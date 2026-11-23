using System.Collections.Immutable;
using System.Globalization;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace Nekotia.Compiler;

sealed partial class CppEmitter
{
    ITypeSymbol? TypeOf(SyntaxNode node) => _model.GetTypeInfo(node).Type;

    string Unsupported(SyntaxNode node, string what, string? hint = null)
    {
        _diag.Error("NEKO0300", $"{what} is not supported in kernel code" + (hint is null ? "" : $" — {hint}"), node);
        return "0 /* unsupported */";
    }

    string NotAvailable(SyntaxNode node, ISymbol api)
    {
        _diag.Error("NEKO0200", $"'{api.ToDisplayString()}' is a .NET API and does not exist in the kernel — use the Nekotia SDK (Nekotia.Hal, Nekotia.Mem) or add a [Native] wrapper", node);
        return "0 /* unavailable */";
    }

    /// <summary>
    /// Emits <paramref name="e"/> converted to the type C# converts it to in this position.
    /// C# and C++ promote integers differently (uint + int is long in C#, unsigned in C++),
    /// so implicit numeric conversions are spelled out as casts.
    /// </summary>
    string ExprAs(ExpressionSyntax e)
    {
        var text = Expr(e);
        var info = _model.GetTypeInfo(e);
        if (info.Type is null || info.ConvertedType is null || SymbolEqualityComparer.Default.Equals(info.Type, info.ConvertedType))
            return text;
        if (info.ConvertedType.TypeKind == TypeKind.Enum && TypeMapper.IsNumeric(info.Type))
            return $"(({Map(info.ConvertedType, e)})({text}))"; // C# lets the literal 0 convert to any enum
        if (!TypeMapper.IsNumeric(info.Type) || !TypeMapper.IsNumeric(info.ConvertedType))
            return text;
        var cv = _model.GetConstantValue(e);
        if (cv.HasValue && cv.Value is char or byte or ushort or uint or ulong ||
            cv.HasValue && cv.Value is sbyte or short or int or long && Convert.ToInt64(cv.Value, CultureInfo.InvariantCulture) >= 0)
            return text; // non-negative literals mean the same in both languages
        return $"(({Map(info.ConvertedType, e)})({text}))";
    }

    string Expr(ExpressionSyntax e)
    {
        switch (e)
        {
            case ParenthesizedExpressionSyntax p:
                return "(" + Expr(p.Expression) + ")";
            case LiteralExpressionSyntax lit:
                return Literal(lit);
            case IdentifierNameSyntax or GenericNameSyntax or QualifiedNameSyntax or AliasQualifiedNameSyntax:
                return Name(e);
            case PredefinedTypeSyntax:
                return Map(TypeOf(e), e);
            case MemberAccessExpressionSyntax ma:
                return MemberAccess(ma);
            case InvocationExpressionSyntax inv:
                return Invocation(inv);
            case AssignmentExpressionSyntax a:
                return Assignment(a);
            case BinaryExpressionSyntax b:
                return Binary(b);
            case PrefixUnaryExpressionSyntax pu:
                return PrefixUnary(pu);
            case PostfixUnaryExpressionSyntax { RawKind: (int)SyntaxKind.SuppressNullableWarningExpression } po:
                return Expr(po.Operand);
            case PostfixUnaryExpressionSyntax po:
                RejectProperty(po.Operand, "++/-- on a property");
                return Expr(po.Operand) + po.OperatorToken.Text;
            case CastExpressionSyntax c:
                return $"(({Map(TypeOf(c.Type), c.Type)})({Expr(c.Expression)}))";
            case ConditionalExpressionSyntax ce:
                return $"({Expr(ce.Condition)} ? {ExprAs(ce.WhenTrue)} : {ExprAs(ce.WhenFalse)})";
            case ElementAccessExpressionSyntax ea:
                return ElementAccess(ea);
            case SizeOfExpressionSyntax so:
                return $"((int32_t)sizeof({Map(TypeOf(so.Type), so.Type)}))";
            case DefaultExpressionSyntax de:
                return Map(TypeOf(de.Type), de.Type) + "{}";
            case ThisExpressionSyntax:
                return "(*this)";
            case CheckedExpressionSyntax ch:
                return "(" + Expr(ch.Expression) + ")";
            case ObjectCreationExpressionSyntax oc:
                return ObjectCreation(oc, oc.Initializer);
            case ImplicitObjectCreationExpressionSyntax ioc:
                return ObjectCreation(ioc, ioc.Initializer);
            case StackAllocArrayCreationExpressionSyntax sa:
                return StackAlloc(sa);
            case RefExpressionSyntax r:
                return Expr(r.Expression);
            case InterpolatedStringExpressionSyntax:
                return Unsupported(e, "string interpolation", "write the parts with Serial.Write / Serial.WriteDec");
            case BaseExpressionSyntax:
                return Unsupported(e, "base access");
            case ThrowExpressionSyntax:
                return Unsupported(e, "throw expressions", "use a throw statement");
            case LambdaExpressionSyntax or AnonymousMethodExpressionSyntax:
                return Unsupported(e, "lambdas", "use a static method and a function pointer (&Method)");
            case IsPatternExpressionSyntax or SwitchExpressionSyntax:
                return Unsupported(e, "pattern matching");
            default:
                return Unsupported(e, e.Kind().ToString());
        }
    }

    // ---------------------------------------------------------------- names & members

    string Name(ExpressionSyntax e)
    {
        var symbol = _model.GetSymbolInfo(e).Symbol;
        switch (symbol)
        {
            case ILocalSymbol l:
                return Symbols.Escape(l.Name);
            case IParameterSymbol p:
                return Symbols.Escape(p.Name);
            case IFieldSymbol f:
                return FieldRef(f, "this", "->", e);
            case IPropertySymbol p:
                return PropertyGet(p, "this", "->", e);
            case ITypeSymbol t:
                return Map(t, e);
            case IMethodSymbol m:
                return MethodRef(m, e);
            default:
                return Unsupported(e, $"name '{e}'");
        }
    }

    string MemberAccess(MemberAccessExpressionSyntax ma)
    {
        var symbol = _model.GetSymbolInfo(ma).Symbol;
        bool pointer = ma.IsKind(SyntaxKind.PointerMemberAccessExpression);

        string Receiver(out string sep)
        {
            if (ma.Expression is ThisExpressionSyntax)
            {
                sep = "->";
                return "this";
            }
            sep = pointer ? "->" : ".";
            return Expr(ma.Expression);
        }

        switch (symbol)
        {
            case IFieldSymbol f:
            {
                if (f.IsStatic || f.IsConst || f.ContainingType.TypeKind == TypeKind.Enum)
                    return FieldRef(f, null, null, ma);
                var recv = Receiver(out var sep);
                return FieldRef(f, recv, sep, ma);
            }
            case IPropertySymbol { Name: "Length", ContainingType.SpecialType: SpecialType.System_String }
                when _model.GetConstantValue(ma.Expression).Value is string constant:
                return constant.Length.ToString(CultureInfo.InvariantCulture); // "abc".Length of a constant
            case IPropertySymbol p:
            {
                if (p.IsStatic)
                    return PropertyGet(p, null, null, ma);
                var recv = Receiver(out var sep);
                return PropertyGet(p, recv, sep, ma);
            }
            case ITypeSymbol t:
                return Map(t, ma);
            case IMethodSymbol m:
                return MethodRef(m, ma);
            default:
                return Unsupported(ma, $"member access '{ma}'");
        }
    }

    string FieldRef(IFieldSymbol f, string? receiver, string? sep, SyntaxNode at)
    {
        if (f.IsConst && !Symbols.IsInSource(f))
            return Constant(f.ConstantValue, f.Type); // int.MaxValue and friends
        if (!Symbols.IsInSource(f))
            return NotAvailable(at, f);
        if (Symbols.IsIntrinsicType(f.ContainingType))
            return Unsupported(at, $"'{f.Name}' outside of its sugar form");
        if (f.ContainingType.TypeKind == TypeKind.Enum)
            return $"{Qualified(f.ContainingType)}::{Symbols.Escape(f.Name)}";
        if (f.IsStatic || f.IsConst)
            return Symbols.NativeName(f) ?? $"{Qualified(f.ContainingType)}::{FieldName(f)}";
        return $"{receiver}{sep}{FieldName(f)}";
    }

    string PropertyGet(IPropertySymbol p, string? receiver, string? sep, SyntaxNode at)
    {
        if (!Symbols.IsInSource(p))
            return NotAvailable(at, p);
        var call = $"get_{Symbols.Escape(p.Name)}()";
        return p.IsStatic ? $"{Qualified(p.ContainingType)}::{call}" : $"{receiver}{sep}{call}";
    }

    string MethodRef(IMethodSymbol m, SyntaxNode at)
    {
        if (Symbols.NativeName(m) is { } native)
            return native;
        if (!m.IsStatic)
            return Unsupported(at, "pointers to instance methods");
        return $"{Qualified(m.ContainingType)}::{Symbols.Escape(m.Name)}";
    }

    void RejectProperty(ExpressionSyntax target, string what)
    {
        if (_model.GetSymbolInfo(target).Symbol is IPropertySymbol)
            Unsupported(target, what);
    }

    // ---------------------------------------------------------------- calls

    string Invocation(InvocationExpressionSyntax inv)
    {
        var constant = _model.GetConstantValue(inv);
        if (constant.HasValue)
            return Constant(constant.Value, TypeOf(inv)); // nameof(...)

        var op = _model.GetOperation(inv);
        if (op is IFunctionPointerInvocationOperation fpi)
        {
            var sig = fpi.GetFunctionPointerSignature();
            return $"{Expr(inv.Expression)}({Arguments(fpi.Arguments, sig)})";
        }
        if (op is not IInvocationOperation call)
            return Unsupported(inv, "this call");

        var m = call.TargetMethod;
        if (Symbols.IsIntrinsicType(m.ContainingType))
            return Intrinsic(m, inv, call);
        if (inv.ArgumentList.Arguments is [{ Expression: InterpolatedStringExpressionSyntax interpolated }])
            return InterpolatedWrite(m, interpolated, inv);

        var args = Arguments(call.Arguments, m);
        if (Symbols.NativeName(m) is { } native)
            return $"{native}({args})";
        if (!Symbols.IsInSource(m))
            return NotAvailable(inv, m);

        var name = Symbols.Escape(m.Name) + TemplateArguments(m, inv);
        if (m.IsStatic)
            return $"{Qualified(m.ContainingType)}::{name}({args})";

        string receiver = inv.Expression switch
        {
            MemberAccessExpressionSyntax { Expression: ThisExpressionSyntax } => "this->",
            MemberAccessExpressionSyntax ma when ma.IsKind(SyntaxKind.PointerMemberAccessExpression) => Expr(ma.Expression) + "->",
            MemberAccessExpressionSyntax ma => Expr(ma.Expression) + ".",
            _ => "this->",
        };
        return $"{receiver}{name}({args})";
    }

    string TemplateArguments(IMethodSymbol m, SyntaxNode at) => m.IsGenericMethod
        ? "<" + string.Join(", ", m.TypeArguments.Select(t => Map(t, at))) + ">"
        : "";

    string Arguments(ImmutableArray<IArgumentOperation> args, IMethodSymbol target)
    {
        // With overloads C++ may pick a different candidate than C# did, so pin every argument to the chosen parameter type.
        bool pin = target.ContainingType is { } ct && ct.GetMembers(target.Name).Length > 1;
        return string.Join(", ", args.OrderBy(a => a.Parameter?.Ordinal ?? 0).Select(a => Argument(a, pin)));
    }

    string Argument(IArgumentOperation a, bool pin)
    {
        var param = a.Parameter;
        if (a.ArgumentKind == ArgumentKind.ParamArray)
            return Unsupported(a.Syntax, "params arguments");
        if (a.ArgumentKind == ArgumentKind.DefaultValue && param is not null && CallerInfo(param, a.Syntax) is { } info)
            return info;
        if (a.ArgumentKind == ArgumentKind.DefaultValue)
            return a.Value.ConstantValue.HasValue ? Constant(a.Value.ConstantValue.Value, param?.Type) : Map(param?.Type, a.Syntax) + "{}";

        var expr = a.Syntax is ArgumentSyntax s ? s.Expression : a.Syntax as ExpressionSyntax;
        if (expr is null)
            return Unsupported(a.Syntax, "this argument");
        if (expr is DeclarationExpressionSyntax)
            return Unsupported(expr, "out variable declarations", "declare the variable before the call");
        if (param is null || param.RefKind != RefKind.None)
            return Expr(expr);

        var type = TypeOf(expr);
        if (pin && type is not null && !SymbolEqualityComparer.Default.Equals(type, param.Type) && TypeMapper.IsNumeric(type))
            return $"(({Map(param.Type, expr)})({Expr(expr)}))";
        return ExprAs(expr);
    }

    /// <summary>[CallerFilePath] / [CallerLineNumber] / [CallerMemberName]: repository-relative path and the full C# method name.</summary>
    string? CallerInfo(IParameterSymbol param, SyntaxNode callSite)
    {
        foreach (var attr in param.GetAttributes())
        {
            switch (attr.AttributeClass?.ToDisplayString())
            {
                case "System.Runtime.CompilerServices.CallerFilePathAttribute":
                    return CString(RelativePath(callSite.SyntaxTree.FilePath));
                case "System.Runtime.CompilerServices.CallerLineNumberAttribute":
                    return (callSite.GetLocation().GetLineSpan().StartLinePosition.Line + 1).ToString(CultureInfo.InvariantCulture);
                case "System.Runtime.CompilerServices.CallerMemberNameAttribute":
                    return CString(EnclosingMember(callSite));
            }
        }
        return null;
    }

    string Intrinsic(IMethodSymbol m, InvocationExpressionSyntax inv, IInvocationOperation call)
    {
        switch ($"{m.ContainingType.Name}.{m.Name}")
        {
            case "Asm.Emit":
            case "Asm.EmitAtt":
            {
                var arg = inv.ArgumentList.Arguments.FirstOrDefault();
                if (arg is null || _model.GetConstantValue(arg.Expression).Value is not string code)
                {
                    _diag.Error("NEKO0302", "assembly must be a compile-time constant string", inv);
                    return "((void)0)";
                }
                var macro = m.Name == "Emit" ? "NEKO_ASM_INTEL" : "NEKO_ASM";
                return $"{macro}({CString(code.Replace("%", "%%"))})";
            }
            case "Intrinsics.Unreachable":
                return "__builtin_unreachable()";
            case "Intrinsics.Breakpoint":
                return "NEKO_ASM(\"int3\")";
            case "Intrinsics.CompilerBarrier":
                return "NEKO_ASM(\"\")";
            default:
                return Unsupported(inv, $"intrinsic '{m.ToDisplayString()}'");
        }
    }

    // ---------------------------------------------------------------- operators

    string Assignment(AssignmentExpressionSyntax a)
    {
        if (_model.GetSymbolInfo(a.Left).Symbol is IPropertySymbol p)
        {
            if (!a.IsKind(SyntaxKind.SimpleAssignmentExpression))
                return Unsupported(a, "compound assignment to a property");
            var value = ExprAs(a.Right);
            var set = $"set_{Symbols.Escape(p.Name)}({value})";
            if (p.IsStatic)
                return $"{Qualified(p.ContainingType)}::{set}";
            return a.Left switch
            {
                MemberAccessExpressionSyntax { Expression: ThisExpressionSyntax } => "this->" + set,
                MemberAccessExpressionSyntax ma when ma.IsKind(SyntaxKind.PointerMemberAccessExpression) => Expr(ma.Expression) + "->" + set,
                MemberAccessExpressionSyntax ma => Expr(ma.Expression) + "." + set,
                _ => "this->" + set,
            };
        }

        if (a.IsKind(SyntaxKind.CoalesceAssignmentExpression) || a.IsKind(SyntaxKind.UnsignedRightShiftAssignmentExpression))
            return Unsupported(a, $"operator {a.OperatorToken.Text}");
        if (a.Left is TupleExpressionSyntax)
            return Unsupported(a, "deconstruction");

        return $"{Expr(a.Left)} {a.OperatorToken.Text} {ExprAs(a.Right)}";
    }

    string Binary(BinaryExpressionSyntax b)
    {
        switch (b.Kind())
        {
            case SyntaxKind.CoalesceExpression:
            case SyntaxKind.AsExpression:
            case SyntaxKind.IsExpression:
            case SyntaxKind.UnsignedRightShiftExpression:
                return Unsupported(b, $"operator {b.OperatorToken.Text}");
        }

        var lt = TypeOf(b.Left);
        var rt = TypeOf(b.Right);
        bool leftString = lt?.SpecialType == SpecialType.System_String;
        bool rightString = rt?.SpecialType == SpecialType.System_String;
        if (b.IsKind(SyntaxKind.AddExpression) && (leftString || rightString))
            return Unsupported(b, "string concatenation", "call Serial.Write for each part");
        if ((b.IsKind(SyntaxKind.EqualsExpression) || b.IsKind(SyntaxKind.NotEqualsExpression)) && leftString && rightString)
            return Unsupported(b, "string comparison", "use Str.Equals(a, b)");

        return $"({ExprAs(b.Left)} {b.OperatorToken.Text} {ExprAs(b.Right)})";
    }

    string PrefixUnary(PrefixUnaryExpressionSyntax pu)
    {
        switch (pu.Kind())
        {
            case SyntaxKind.AddressOfExpression when _model.GetSymbolInfo(pu.Operand).Symbol is IMethodSymbol m:
                return "&" + MethodRef(m, pu);
            case SyntaxKind.PreIncrementExpression:
            case SyntaxKind.PreDecrementExpression:
                RejectProperty(pu.Operand, "++/-- on a property");
                return pu.OperatorToken.Text + Expr(pu.Operand);
            case SyntaxKind.AddressOfExpression:
            case SyntaxKind.PointerIndirectionExpression:
                return $"({pu.OperatorToken.Text}{Expr(pu.Operand)})";
            case SyntaxKind.IndexExpression:
                return Unsupported(pu, "index-from-end (^)");
            default:
                return $"({pu.OperatorToken.Text}{ExprAs(pu.Operand)})";
        }
    }

    string ElementAccess(ElementAccessExpressionSyntax ea)
    {
        // Indexing a compile-time string: a UTF-16 literal has exactly C#'s char semantics.
        if (_model.GetConstantValue(ea.Expression).Value is string table && ea.ArgumentList.Arguments.Count == 1)
            return $"{Utf16Literal(table)}[{Expr(ea.ArgumentList.Arguments[0].Expression)}]";
        if (TypeOf(ea.Expression) is not IPointerTypeSymbol)
            return Unsupported(ea, $"indexing a '{TypeOf(ea.Expression)?.ToDisplayString()}'", "only pointers and fixed buffers can be indexed");
        if (ea.ArgumentList.Arguments.Count != 1)
            return Unsupported(ea, "multi-dimensional indexing");
        return $"{Expr(ea.Expression)}[{Expr(ea.ArgumentList.Arguments[0].Expression)}]";
    }

    // ---------------------------------------------------------------- creation

    string ObjectCreation(BaseObjectCreationExpressionSyntax oc, InitializerExpressionSyntax? init)
    {
        if (TypeOf(oc) is not INamedTypeSymbol { TypeKind: TypeKind.Struct } type)
            return Unsupported(oc, "creating reference types", "use a struct; allocate on the heap with Heap.New<T>()");
        if (_model.GetOperation(oc) is not IObjectCreationOperation op)
            return Unsupported(oc, "this object creation");

        var t = Map(type, oc);
        var ctor = op.Constructor;
        var core = ctor is null || ctor.IsImplicitlyDeclared || ctor.Parameters.Length == 0 && !Symbols.IsInSource(ctor)
            ? t + "{}"
            : $"{t}({Arguments(op.Arguments, ctor)})";
        if (init is null || init.Expressions.Count == 0)
            return core;

        var sb = new StringBuilder();
        sb.Append(_inFunction ? "[&]() { " : "[]() { ").Append(t).Append(" _o = ").Append(core).Append("; ");
        foreach (var item in init.Expressions)
        {
            if (item is not AssignmentExpressionSyntax { Left: IdentifierNameSyntax } asg)
            {
                Unsupported(item, "this kind of object initializer");
                continue;
            }
            var value = ExprAs(asg.Right);
            switch (_model.GetSymbolInfo(asg.Left).Symbol)
            {
                case IFieldSymbol f:
                    sb.Append($"_o.{FieldName(f)} = {value}; ");
                    break;
                case IPropertySymbol p:
                    sb.Append($"_o.set_{Symbols.Escape(p.Name)}({value}); ");
                    break;
                default:
                    Unsupported(asg.Left, "this initializer target");
                    break;
            }
        }
        sb.Append("return _o; }()");
        return sb.ToString();
    }

    string StackAlloc(StackAllocArrayCreationExpressionSyntax sa)
    {
        if (TypeOf(sa) is not IPointerTypeSymbol pointer)
            return Unsupported(sa, "stackalloc into a Span", "assign it to a pointer: byte* buf = stackalloc byte[64];");
        if (sa.Initializer is not null || sa.Type is not ArrayTypeSyntax { RankSpecifiers: [{ Sizes: [var size] }] } || size is OmittedArraySizeExpressionSyntax)
            return Unsupported(sa, "stackalloc with an initializer");

        // C# zero-fills stackalloc memory; alloca must run in this frame, hence a GNU statement expression.
        var elem = Map(pointer.PointedAtType, sa);
        return $"({{ size_t _n = (size_t)({Expr(size)}) * sizeof({elem}); void* _p = __builtin_alloca(_n); __builtin_memset(_p, 0, _n); ({elem}*)_p; }})";
    }

    // ---------------------------------------------------------------- literals & constants

    string Literal(LiteralExpressionSyntax lit)
    {
        switch (lit.Kind())
        {
            case SyntaxKind.NumericLiteralExpression:
                return NumericLiteral(lit);
            case SyntaxKind.StringLiteralExpression:
            case SyntaxKind.Utf8StringLiteralExpression:
                return CString((string)lit.Token.Value!);
            case SyntaxKind.CharacterLiteralExpression:
                return CharLiteral((char)lit.Token.Value!);
            case SyntaxKind.TrueLiteralExpression:
                return "true";
            case SyntaxKind.FalseLiteralExpression:
                return "false";
            case SyntaxKind.NullLiteralExpression:
                return "nullptr";
            case SyntaxKind.DefaultLiteralExpression:
                return Map(_model.GetTypeInfo(lit).ConvertedType, lit) + "{}";
            default:
                return Unsupported(lit, lit.Kind().ToString());
        }
    }

    string NumericLiteral(LiteralExpressionSyntax lit)
    {
        var type = TypeOf(lit);
        if (type?.SpecialType is SpecialType.System_Single or SpecialType.System_Double or SpecialType.System_Decimal)
            return Map(type, lit); // reports the floating point error

        var text = lit.Token.Text.TrimEnd('u', 'U', 'l', 'L').Replace('_', '\'');
        text = text.Replace("0x'", "0x").Replace("0X'", "0X").Replace("0b'", "0b").Replace("0B'", "0B");
        return text + type?.SpecialType switch
        {
            SpecialType.System_UInt32 => "U",
            SpecialType.System_Int64 => "L",
            SpecialType.System_UInt64 => "UL",
            _ => "",
        };
    }

    string Constant(object? value, ITypeSymbol? type)
    {
        if (type is INamedTypeSymbol { TypeKind: TypeKind.Enum } e)
            return $"(({Qualified(e)})({Constant(value, e.EnumUnderlyingType)}))";

        switch (value)
        {
            case null:
                return type is null or IPointerTypeSymbol || type.SpecialType == SpecialType.System_String || type.IsReferenceType
                    ? "nullptr"
                    : Map(type, null) + "{}";
            case bool b:
                return b ? "true" : "false";
            case string s:
                return CString(s);
            case char c:
                return CharLiteral(c);
            case int i:
                return i == int.MinValue ? "(-2147483647 - 1)" : i.ToString(CultureInfo.InvariantCulture);
            case uint u:
                return u.ToString(CultureInfo.InvariantCulture) + "U";
            case long l:
                return l == long.MinValue ? "(-9223372036854775807L - 1)" : l.ToString(CultureInfo.InvariantCulture) + "L";
            case ulong ul:
                return ul.ToString(CultureInfo.InvariantCulture) + "UL";
            case sbyte or byte or short or ushort:
                return Convert.ToInt32(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture);
            default:
                _diag.Error("NEKO0201", $"constant of type {value.GetType().Name} is not supported", (Location?)null);
                return "0";
        }
    }

    /// <summary>u"..." literal with the same UTF-16 code units as the C# string.</summary>
    static string Utf16Literal(string s)
    {
        var sb = new StringBuilder("u\"");
        for (int i = 0; i < s.Length; i++)
        {
            char c = s[i];
            if (char.IsHighSurrogate(c) && i + 1 < s.Length && char.IsLowSurrogate(s[i + 1]))
            {
                sb.Append($"\\U{char.ConvertToUtf32(c, s[++i]):X8}");
                continue;
            }
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case >= ' ' and <= '~': sb.Append(c); break;
                case < (char)0x80: sb.Append('\\').Append(Convert.ToString(c, 8).PadLeft(3, '0')); break;
                default: sb.Append($"\\u{(int)c:X4}"); break;
            }
        }
        return sb.Append('"').ToString();
    }

    // ---------------------------------------------------------------- string interpolation

    /// <summary>
    /// <c>Terminal.WriteLine($"took {ms} ms")</c> → <c>(Terminal::Write("took "), Terminal::WriteDec(ms), Terminal::Write(" ms"), Terminal::WriteLine())</c>.
    /// The target type must offer the Write family: Write(string), WriteDec(long/ulong), and optionally WriteHex, WriteChar,
    /// WriteCodepoint, WriteSize, WriteLine(). No string is ever built, so this works without a heap.
    /// Format specifiers: {x:X} hex, {n:size} human-readable bytes.
    /// </summary>
    string InterpolatedWrite(IMethodSymbol target, InterpolatedStringExpressionSyntax interpolated, InvocationExpressionSyntax inv)
    {
        var type = target.ContainingType;
        if (target.Name is not ("Write" or "WriteLine"))
            return Unsupported(inv, "string interpolation", "it is only supported as the argument of Write/WriteLine on Serial, Terminal or a type with the same Write* methods");

        IMethodSymbol? Find(string name, SpecialType? param)
        {
            foreach (var m in type.GetMembers(name).OfType<IMethodSymbol>())
            {
                if (param is null && m.Parameters.Length == 0)
                    return m;
                if (param is not null && m.Parameters.Length == 1 && m.Parameters[0].Type.SpecialType == param)
                    return m;
            }
            return null;
        }

        string Call(IMethodSymbol? m, string arg, SyntaxNode at, string what)
        {
            if (m is null)
            {
                Unsupported(at, $"interpolating {what}", $"'{type.Name}' has no matching Write* method");
                return "((void)0)";
            }
            var callee = Symbols.NativeName(m) ?? $"{Qualified(m.ContainingType)}::{Symbols.Escape(m.Name)}";
            var cast = m.Parameters.Length == 1 && m.Parameters[0].Type.SpecialType != SpecialType.System_String
                ? $"(({Map(m.Parameters[0].Type, at)})({arg}))"
                : arg;
            return $"{callee}({cast})";
        }

        var parts = new List<string>();
        var writeString = Find("Write", SpecialType.System_String);
        foreach (var content in interpolated.Contents)
        {
            if (content is InterpolatedStringTextSyntax text)
            {
                parts.Add(Call(writeString, CString(text.TextToken.ValueText), text, "text"));
                continue;
            }

            var hole = (InterpolationSyntax)content;
            if (hole.AlignmentClause is not null)
                Unsupported(hole.AlignmentClause, "interpolation alignment");
            var format = hole.FormatClause?.FormatStringToken.ValueText;
            var value = Expr(hole.Expression);
            var t = TypeOf(hole.Expression);
            if (t is INamedTypeSymbol { TypeKind: TypeKind.Enum } e)
                t = e.EnumUnderlyingType;

            switch (t)
            {
                case { SpecialType: SpecialType.System_String }:
                case IPointerTypeSymbol { PointedAtType.SpecialType: SpecialType.System_Byte }:
                    parts.Add(Call(t is IPointerTypeSymbol ? FindPointerWrite(type) : writeString, value, hole, "a string"));
                    break;
                case { SpecialType: SpecialType.System_Boolean }:
                    parts.Add(Call(writeString, $"(({value}) ? \"true\" : \"false\")", hole, "a bool"));
                    break;
                case { SpecialType: SpecialType.System_Char }:
                    parts.Add(Call(Find("WriteCodepoint", SpecialType.System_UInt32) ?? Find("WriteChar", SpecialType.System_Byte), value, hole, "a char"));
                    break;
                case { SpecialType: SpecialType.System_SByte or SpecialType.System_Int16 or SpecialType.System_Int32 or SpecialType.System_Int64 or SpecialType.System_IntPtr }
                    when format is null:
                    parts.Add(Call(Find("WriteDec", SpecialType.System_Int64), value, hole, "an integer"));
                    break;
                case var _ when TypeMapper.IsNumeric(t) || t is IPointerTypeSymbol:
                    var arg = t is IPointerTypeSymbol ? $"(uint64_t)(uintptr_t)({value})" : value;
                    var method = format switch
                    {
                        null => Find("WriteDec", SpecialType.System_UInt64),
                        "X" or "x" => Find("WriteHex", SpecialType.System_UInt64),
                        "size" => Find("WriteSize", SpecialType.System_UInt64),
                        _ => null,
                    };
                    if (format is not null and not ("X" or "x" or "size"))
                        Unsupported(hole.FormatClause!, $"format ':{format}'", "supported: :X (hex), :size (bytes)");
                    parts.Add(Call(method, arg, hole, "a number"));
                    break;
                default:
                    parts.Add(Unsupported(hole.Expression, $"interpolating a value of type '{t?.ToDisplayString()}'"));
                    break;
            }
        }

        if (target.Name == "WriteLine")
            parts.Add(Find("WriteLine", null) is { } newline
                ? Call(newline, "", inv, "a newline")
                : Call(writeString, "\"\\n\"", inv, "a newline"));
        return "(" + string.Join(", ", parts) + ")";
    }

    IMethodSymbol? FindPointerWrite(INamedTypeSymbol type) =>
        type.GetMembers("Write").OfType<IMethodSymbol>().FirstOrDefault(m =>
            m.Parameters is [{ Type: IPointerTypeSymbol { PointedAtType.SpecialType: SpecialType.System_Byte } }]);

    static string CharLiteral(char c) => c is >= ' ' and <= '~' and not '\'' and not '\\'
        ? $"u'{c}'"
        : $"((char16_t)0x{(int)c:X})";

    /// <summary>A C string literal holding the UTF-8 bytes of <paramref name="s"/>.</summary>
    static string CString(string s)
    {
        var sb = new StringBuilder("\"");
        foreach (var b in Encoding.UTF8.GetBytes(s))
        {
            switch (b)
            {
                case (byte)'"': sb.Append("\\\""); break;
                case (byte)'\\': sb.Append("\\\\"); break;
                case (byte)'\n': sb.Append("\\n"); break;
                case (byte)'\r': sb.Append("\\r"); break;
                case (byte)'\t': sb.Append("\\t"); break;
                case >= 0x20 and < 0x7F: sb.Append((char)b); break;
                default: sb.Append('\\').Append(Convert.ToString(b, 8).PadLeft(3, '0')); break;
            }
        }
        return sb.Append('"').ToString();
    }
}
