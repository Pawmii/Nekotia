using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace Nekotia.Compiler;

sealed partial class CppEmitter
{
    void Statement(CodeWriter w, StatementSyntax s)
    {
        if (s is not BlockSyntax)
            LineDirective(w, s);

        switch (s)
        {
            case BlockSyntax block:
                w.OpenBrace();
                foreach (var inner in block.Statements)
                    Statement(w, inner);
                w.CloseBrace();
                break;

            case LocalDeclarationStatementSyntax local:
                LocalDeclaration(w, local);
                break;

            case ExpressionStatementSyntax es:
                w.Line(Expr(es.Expression) + ";");
                break;

            case IfStatementSyntax ifs when IsDeferMarker(ifs.Condition):
                Defer(w, ifs);
                break;

            case IfStatementSyntax ifs:
                w.Line($"if ({Expr(ifs.Condition)})");
                Embedded(w, ifs.Statement);
                if (ifs.Else is { } els)
                {
                    w.Line("else");
                    Embedded(w, els.Statement);
                }
                break;

            case WhileStatementSyntax ws:
                w.Line($"while ({Expr(ws.Condition)})");
                Embedded(w, ws.Statement);
                break;

            case DoStatementSyntax ds:
                w.Line("do");
                Embedded(w, ds.Statement);
                w.Line($"while ({Expr(ds.Condition)});");
                break;

            case ForStatementSyntax fs:
                For(w, fs);
                break;

            case ReturnStatementSyntax rs:
                w.Line(rs.Expression is null ? "return;" : $"return {ExprAs(rs.Expression)};");
                break;

            case BreakStatementSyntax:
                w.Line("break;");
                break;

            case ContinueStatementSyntax:
                w.Line("continue;");
                break;

            case SwitchStatementSyntax sw:
                Switch(w, sw);
                break;

            case EmptyStatementSyntax:
                w.Line(";");
                break;

            case UnsafeStatementSyntax us:
                Statement(w, us.Block);
                break;

            case CheckedStatementSyntax cs:
                Statement(w, cs.Block);
                break;

            case FixedStatementSyntax fx:
                // No GC → nothing moves → pinning is just a pointer declaration.
                w.OpenBrace();
                VariableDeclaration(w, fx.Declaration, isConst: false);
                Embedded(w, fx.Statement);
                w.CloseBrace();
                break;

            case LabeledStatementSyntax ls:
                w.Line($"{Symbols.Escape(ls.Identifier.Text)}:");
                Statement(w, ls.Statement);
                break;

            case GotoStatementSyntax { RawKind: (int)SyntaxKind.GotoStatement, Expression: IdentifierNameSyntax label }:
                w.Line($"goto {Symbols.Escape(label.Identifier.Text)};");
                break;

            case ThrowStatementSyntax th:
                w.Line(Throw(th) + ";");
                break;

            case TryStatementSyntax:
                Unsupported(s, "try/catch/finally", "not yet: an uncaught `throw` shows the stop screen; use `defer` (in .neko files) for cleanup");
                break;

            case ForEachStatementSyntax:
                Unsupported(s, "foreach", "use `for i in a..b { }` in .neko files or a plain for loop");
                break;

            case LocalFunctionStatementSyntax:
                Unsupported(s, "local functions", "make it a private static method");
                break;

            default:
                Unsupported(s, s.Kind().ToString());
                break;
        }
    }

    void Embedded(CodeWriter w, StatementSyntax s)
    {
        if (s is BlockSyntax)
        {
            Statement(w, s);
            return;
        }
        w.OpenBrace();
        Statement(w, s);
        w.CloseBrace();
    }

    void LocalDeclaration(CodeWriter w, LocalDeclarationStatementSyntax local)
    {
        if (local.UsingKeyword.IsKind(SyntaxKind.UsingKeyword))
        {
            Unsupported(local, "using declarations", "use `defer` in .neko files");
            return;
        }
        VariableDeclaration(w, local.Declaration, local.IsConst);
    }

    void VariableDeclaration(CodeWriter w, VariableDeclarationSyntax decl, bool isConst)
    {
        if (decl.Type is RefTypeSyntax)
        {
            Unsupported(decl, "ref locals");
            return;
        }
        var type = Map(_model.GetTypeInfo(decl.Type).Type, decl.Type);
        var prefix = isConst ? "constexpr " : "";   // C# const locals are compile-time constants
        foreach (var v in decl.Variables)
        {
            var name = Symbols.Escape(v.Identifier.Text);
            w.Line(v.Initializer is null
                ? $"{prefix}{type} {name}{{}};"
                : $"{prefix}{type} {name} = {ExprAs(v.Initializer.Value)};");
        }
    }

    void For(CodeWriter w, ForStatementSyntax fs)
    {
        string init = "";
        if (fs.Declaration is { } decl)
        {
            var type = Map(_model.GetTypeInfo(decl.Type).Type, decl.Type);
            if (decl.Variables.Count > 1 && type.EndsWith('*'))
                Unsupported(decl, "several pointer variables in one for-initializer");
            init = type + " " + string.Join(", ", decl.Variables.Select(v =>
                Symbols.Escape(v.Identifier.Text) + (v.Initializer is null ? "{}" : " = " + ExprAs(v.Initializer.Value))));
        }
        else if (fs.Initializers.Count > 0)
        {
            init = string.Join(", ", fs.Initializers.Select(Expr));
        }

        var cond = fs.Condition is null ? "" : Expr(fs.Condition);
        var incr = string.Join(", ", fs.Incrementors.Select(Expr));
        w.Line($"for ({init}; {cond}; {incr})");
        Embedded(w, fs.Statement);
    }

    void Switch(CodeWriter w, SwitchStatementSyntax sw)
    {
        w.OpenBrace($"switch ({Expr(sw.Expression)})");
        foreach (var section in sw.Sections)
        {
            foreach (var label in section.Labels)
            {
                switch (label)
                {
                    case CaseSwitchLabelSyntax c:
                        w.Line($"case {Expr(c.Value)}:");
                        break;
                    case DefaultSwitchLabelSyntax:
                        w.Line("default:");
                        break;
                    default:
                        Unsupported(label, "pattern case labels");
                        break;
                }
            }
            w.OpenBrace();
            foreach (var s in section.Statements)
                Statement(w, s);
            w.CloseBrace();
        }
        w.CloseBrace();
    }

    // ---------------------------------------------------------------- throw

    /// <summary>
    /// <c>throw new InvalidOperationException("msg")</c> → the stop screen with the exception type, message,
    /// C# file:line and method. There is no try/catch yet, so every throw is unhandled.
    /// </summary>
    string Throw(ThrowStatementSyntax th)
    {
        if (th.Expression is null)
            return Unsupported(th, "rethrow (`throw;`)", "there is no catch yet");
        if (th.Expression is not BaseObjectCreationExpressionSyntax creation || _model.GetOperation(creation) is not IObjectCreationOperation op)
            return Unsupported(th.Expression, "throwing an existing exception object", "write `throw new SomeException(\"message\")`");

        string message = "nullptr";
        foreach (var arg in op.Arguments)
            if (arg.Parameter is { Name: "message", Type.SpecialType: SpecialType.System_String } && arg.ArgumentKind == ArgumentKind.Explicit)
                message = Argument(arg, pin: false);

        var line = th.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
        return $"::neko::crash::unhandled_exception({CString(op.Type!.ToDisplayString())}, {message}, " +
               $"{CString(RelativePath(th.SyntaxTree.FilePath))}, {line}, {CString(EnclosingMember(th))})";
    }

    // ---------------------------------------------------------------- defer (sugar)

    bool IsDeferMarker(ExpressionSyntax condition) =>
        _model.GetSymbolInfo(condition).Symbol is IFieldSymbol { Name: "Defer" } f &&
        Symbols.IsIntrinsicType(f.ContainingType) && f.ContainingType.Name == "Sugar";

    void Defer(CodeWriter w, IfStatementSyntax ifs)
    {
        if (ifs.Else is not null)
            Unsupported(ifs.Else, "else after defer");

        foreach (var node in ifs.Statement.DescendantNodesAndSelf())
        {
            if (node is ReturnStatementSyntax or GotoStatementSyntax)
                _diag.Error("NEKO0301", "a defer body cannot return or goto", node);
            else if (node is BreakStatementSyntax or ContinueStatementSyntax && !HasLoopBetween(node, ifs.Statement))
                _diag.Error("NEKO0301", "a defer body cannot break/continue an outer loop", node);
        }

        var id = _uniqueCounter++;
        w.OpenBrace($"auto _neko_defer_{id} = ::neko::detail::DeferTag{{}} + [&]()");
        if (ifs.Statement is BlockSyntax block)
            foreach (var s in block.Statements)
                Statement(w, s);
        else
            Statement(w, ifs.Statement);
        w.CloseBrace(";");
    }

    static bool HasLoopBetween(SyntaxNode node, SyntaxNode root)
    {
        for (var p = node.Parent; p is not null && p != root.Parent; p = p.Parent)
        {
            if (p is WhileStatementSyntax or DoStatementSyntax or ForStatementSyntax or SwitchStatementSyntax)
                return true;
            if (p == root)
                break;
        }
        return false;
    }
}
