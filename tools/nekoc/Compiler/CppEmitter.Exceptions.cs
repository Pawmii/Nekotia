using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace Nekotia.Compiler;

// try / catch / finally / throw (runtime: neko/exception.hpp).
//
//   throw new T("m")   → ::neko::exc::raise("T\0Base\0System.Exception\0", "m", file, line, method)
//   throw; / throw e;  → ::neko::exc::rethrow(<frame of the enclosing catch>.thrown)
//   try { A } catch (T e) when (F) { B } finally { C }
//     → { auto fin = DeferTag{} + [&]() { C };
//         ::neko::exc::Frame t;  if (neko_setjmp(t.jmp) == 0) { A } else { …catches…, else rethrow } }
//
// setjmp only keeps what is in memory: a local written inside a `try` and declared outside it is
// declared `volatile` (Roslyn data-flow analysis tells which). Functions whose address is taken
// (&Method — C++ may call them) start with a barrier frame: a throw never jumps over C++ frames.
sealed partial class CppEmitter
{
    readonly HashSet<ILocalSymbol> _volatileLocals = new(SymbolEqualityComparer.Default);
    readonly Dictionary<ILocalSymbol, string> _catchVariables = new(SymbolEqualityComparer.Default);
    readonly Stack<string> _catchFrames = new();
    HashSet<IMethodSymbol>? _callbacks;

    /// <summary>Methods used as function pointers anywhere in the program.</summary>
    bool IsCallback(IMethodSymbol m)
    {
        if (_callbacks is null)
        {
            _callbacks = new HashSet<IMethodSymbol>(SymbolEqualityComparer.Default);
            foreach (var tree in _compilation.SyntaxTrees)
            {
                var model = ModelFor(tree);
                foreach (var node in tree.GetRoot().DescendantNodes().OfType<PrefixUnaryExpressionSyntax>())
                    if (node.IsKind(SyntaxKind.AddressOfExpression) && model.GetSymbolInfo(node.Operand).Symbol is IMethodSymbol target)
                        _callbacks.Add(target.OriginalDefinition);
            }
        }
        return _callbacks.Contains(m.OriginalDefinition);
    }

    /// <summary>Per function: which locals must be volatile because a try writes them.</summary>
    void PrepareExceptions(SyntaxNode decl, SyntaxNode? body)
    {
        _volatileLocals.Clear();
        _catchVariables.Clear();
        if (body is null)
            return;
        foreach (var ts in body.DescendantNodesAndSelf().OfType<TryStatementSyntax>())
        {
            if (ts.Catches.Count == 0)
                continue;   // try/finally needs no jump
            var flow = _model.AnalyzeDataFlow(ts.Block);
            if (flow is null || !flow.Succeeded)
                continue;
            foreach (var symbol in flow.WrittenInside)
            {
                bool declaredInside = symbol.DeclaringSyntaxReferences.Any(r => r.SyntaxTree == ts.SyntaxTree && ts.Block.Span.Contains(r.Span));
                if (declaredInside)
                    continue;
                switch (symbol)
                {
                    case ILocalSymbol local when IsScalar(local.Type):
                        _volatileLocals.Add(local);
                        break;
                    case ILocalSymbol local:
                        _diag.Error("NEKO0303", $"'{local.Name}' (a struct) is changed inside a try and declared outside it — after an exception its value would be undefined; declare it inside the try, or use a pointer to it", ts.Block.GetLocation());
                        break;
                    case IParameterSymbol p when p.RefKind == RefKind.None:
                        _diag.Error("NEKO0303", $"parameter '{p.Name}' is changed inside a try — copy it into a local first (locals written in a try are kept in memory)", ts.Block.GetLocation());
                        break;
                }
            }
        }
    }

    static bool IsScalar(ITypeSymbol t) =>
        TypeMapper.IsNumeric(t) || TypeMapper.IsFloating(t) || t is IPointerTypeSymbol || t.TypeKind == TypeKind.Enum ||
        t.SpecialType is SpecialType.System_Boolean or SpecialType.System_Char or SpecialType.System_String;

    bool IsVolatile(ExpressionSyntax e) =>
        e is IdentifierNameSyntax && _model.GetSymbolInfo(e).Symbol is ILocalSymbol l && _volatileLocals.Contains(l);

    /// <summary>"T\0Base\0…\0System.Exception\0" as a C string literal (ends with a double NUL).</summary>
    string ExceptionChain(ITypeSymbol type)
    {
        var names = new List<string>();
        for (var t = type as INamedTypeSymbol; t is not null && t.SpecialType != SpecialType.System_Object; t = t.BaseType)
            names.Add(t.ToDisplayString());
        return CString(string.Join("\0", names) + "\0");
    }

    string Throw(ThrowStatementSyntax th)
    {
        if (th.Expression is null || th.Expression is IdentifierNameSyntax id && _model.GetSymbolInfo(id).Symbol is ILocalSymbol l && _catchVariables.ContainsKey(l))
        {
            if (th.Expression is IdentifierNameSyntax named && _model.GetSymbolInfo(named).Symbol is ILocalSymbol caught)
                return $"::neko::exc::rethrow({_catchVariables[caught]}.thrown)";
            if (_catchFrames.Count == 0)
                return Unsupported(th, "`throw;` outside a catch block");
            return $"::neko::exc::rethrow({_catchFrames.Peek()}.thrown)";
        }
        if (th.Expression is not BaseObjectCreationExpressionSyntax creation || _model.GetOperation(creation) is not IObjectCreationOperation op)
            return Unsupported(th.Expression, "throwing an existing exception object", "write `throw new SomeException(\"message\")`, or `throw;` in a catch");

        string message = "nullptr";
        foreach (var arg in op.Arguments)
            if (arg.Parameter is { Name: "message", Type.SpecialType: SpecialType.System_String } && arg.ArgumentKind == ArgumentKind.Explicit)
                message = Argument(arg, pin: false);

        var line = th.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
        return $"::neko::exc::raise({ExceptionChain(op.Type!)}, {message}, " +
               $"{CString(RelativePath(th.SyntaxTree.FilePath))}, {line}, {CString(EnclosingMember(th))})";
    }

    void Try(CodeWriter w, TryStatementSyntax ts)
    {
        var id = _uniqueCounter++;
        var frame = $"_neko_try{id}";
        w.OpenBrace();
        if (ts.Finally is { } fin)
        {
            foreach (var node in fin.Block.DescendantNodes())
                if (node is ReturnStatementSyntax or GotoStatementSyntax || node is BreakStatementSyntax or ContinueStatementSyntax && !HasLoopBetween(node, fin.Block))
                    _diag.Error("NEKO0301", "a finally block cannot leave it (return, goto, break, continue)", node);
            w.OpenBrace($"auto _neko_finally{id} = ::neko::detail::DeferTag{{}} + [&]()");
            foreach (var s in fin.Block.Statements)
                Statement(w, s);
            w.CloseBrace(";");
        }
        if (ts.Catches.Count == 0)
        {
            Statement(w, ts.Block);
            w.CloseBrace();
            return;
        }

        w.Line($"::neko::exc::Frame {frame};");
        w.OpenBrace($"if (neko_setjmp({frame}.jmp) == 0)");
        foreach (var s in ts.Block.Statements)
            Statement(w, s);
        w.CloseBrace();
        w.OpenBrace("else");
        w.Line($"bool {frame}_caught = false;");
        foreach (var c in ts.Catches)
        {
            var type = c.Declaration is { } d ? _model.GetTypeInfo(d.Type).Type : null;
            var test = type is null || type.ToDisplayString() == "System.Exception"
                ? $"!{frame}_caught"
                : $"!{frame}_caught && ::neko::exc::is({frame}.thrown, {CString(type.ToDisplayString())})";
            w.OpenBrace($"if ({test})");
            if (c.Declaration is { Identifier.Text.Length: > 0 } declaration &&
                _model.GetDeclaredSymbol(declaration) is ILocalSymbol variable)
            {
                w.Line($"const ::neko::exc::Thrown& {Symbols.Escape(variable.Name)} = {frame}.thrown;");
                _catchVariables[variable] = frame;
            }
            if (c.Filter is { } filter)
                w.OpenBrace($"if ({Expr(filter.FilterExpression)})");
            w.Line($"{frame}_caught = true;");
            _catchFrames.Push(frame);
            foreach (var s in c.Block.Statements)
                Statement(w, s);
            _catchFrames.Pop();
            if (c.Filter is not null)
                w.CloseBrace();
            w.CloseBrace();
        }
        w.Line($"if (!{frame}_caught) ::neko::exc::rethrow({frame}.thrown);");
        w.CloseBrace();
        w.CloseBrace();
    }

    /// <summary>`e.Message` on a catch variable.</summary>
    string? ExceptionMember(MemberAccessExpressionSyntax ma, ISymbol member)
    {
        if (member is not IPropertySymbol { Name: "Message" } || member.ContainingType is not { } t || !Symbols.IsExceptionType(t))
            return null;
        return $"({Expr(ma.Expression)}.message)";
    }
}
