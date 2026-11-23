using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Nekotia.Compiler;

// Pattern matching: `x is <pattern>`, switch expressions and pattern case labels.
//
//   constant (incl. null), discard `_`, relational (< <= > >=), `and` / `or` / `not`, parentheses, `when`
//
// The subject is evaluated once: a side-effect-free subject (a local, a parameter, a field chain) is
// used as is; anything else is bound to `_neko_s` inside an immediately invoked lambda.
sealed partial class CppEmitter
{
    /// <summary>C++ condition that is true when <paramref name="subject"/> matches <paramref name="p"/>.</summary>
    string Pattern(PatternSyntax p, string subject)
    {
        switch (p)
        {
            case DiscardPatternSyntax:
                return "true";
            case ParenthesizedPatternSyntax par:
                return Pattern(par.Pattern, subject);
            case UnaryPatternSyntax { RawKind: (int)SyntaxKind.NotPattern } not:
                return $"!({Pattern(not.Pattern, subject)})";
            case BinaryPatternSyntax b:
                var op = b.IsKind(SyntaxKind.AndPattern) ? "&&" : "||";
                return $"({Pattern(b.Left, subject)} {op} {Pattern(b.Right, subject)})";
            case ConstantPatternSyntax c:
                if (c.Expression.IsKind(SyntaxKind.NullLiteralExpression))
                    return $"({subject} == nullptr)";
                if (TypeOf(c.Expression)?.SpecialType == SpecialType.System_String)
                    return Unsupported(c, "string constant patterns", "compare with Str.Equals");
                return $"({subject} == {ExprAs(c.Expression)})";
            case RelationalPatternSyntax r:
                return $"({subject} {r.OperatorToken.Text} {ExprAs(r.Expression)})";
            case DeclarationPatternSyntax or VarPatternSyntax:
                return Unsupported(p, "patterns that declare variables", "match first, then use the value you already have");
            default:
                return Unsupported(p, $"{p.Kind()}", "supported: constants, null, _, < <= > >=, and/or/not, parentheses");
        }
    }

    /// <summary>A subject that can be read several times: no calls, assignments or increments.</summary>
    static bool IsSimple(ExpressionSyntax e) => e switch
    {
        IdentifierNameSyntax or ThisExpressionSyntax or LiteralExpressionSyntax => true,
        ParenthesizedExpressionSyntax p => IsSimple(p.Expression),
        MemberAccessExpressionSyntax m => IsSimple(m.Expression),
        PrefixUnaryExpressionSyntax { RawKind: (int)SyntaxKind.PointerIndirectionExpression } d => IsSimple(d.Operand),
        ElementAccessExpressionSyntax ea => IsSimple(ea.Expression) && ea.ArgumentList.Arguments.All(a => IsSimple(a.Expression)),
        _ => false,
    };

    string IsPattern(IsPatternExpressionSyntax e)
    {
        if (IsSimple(e.Expression))
            return Pattern(e.Pattern, $"({Expr(e.Expression)})");
        return $"([&]() -> bool {{ auto&& _neko_s = {Expr(e.Expression)}; return {Pattern(e.Pattern, "_neko_s")}; }}())";
    }

    string SwitchExpression(SwitchExpressionSyntax sw)
    {
        var type = _model.GetTypeInfo(sw).ConvertedType ?? TypeOf(sw);
        var resultType = Map(type, sw);
        var line = sw.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
        var body = new System.Text.StringBuilder();
        body.Append($"[&]() -> {resultType} {{ auto&& _neko_s = {Expr(sw.GoverningExpression)}; ");
        foreach (var arm in sw.Arms)
        {
            var condition = Pattern(arm.Pattern, "_neko_s");
            if (arm.WhenClause is { } when)
                condition = $"({condition}) && ({Expr(when.Condition)})";
            var value = ExprAs(arm.Expression);
            body.Append(condition == "true" ? $"return {value}; " : $"if ({condition}) return {value}; ");
        }
        body.Append($"::neko::crash::unhandled_exception(\"System.Runtime.CompilerServices.SwitchExpressionException\", " +
                    $"\"no arm of the switch expression matched\", {CString(RelativePath(sw.SyntaxTree.FilePath))}, {line}, " +
                    $"{CString(EnclosingMember(sw))}); }}()");
        return "(" + body + ")";
    }

    static bool NeedsPatternSwitch(SwitchStatementSyntax sw) =>
        sw.Sections.SelectMany(s => s.Labels).Any(l => l is CasePatternSwitchLabelSyntax);

    /// <summary>
    /// A switch statement with pattern labels: `switch (0) { default: if … else if … }` — `break` leaves the
    /// switch and `continue` still reaches the enclosing loop, as in C#.
    /// </summary>
    void PatternSwitch(CodeWriter w, SwitchStatementSyntax sw)
    {
        var id = _uniqueCounter++;
        var subject = $"_neko_s{id}";
        w.OpenBrace("switch (0)");
        w.Line("default:");
        w.OpenBrace();
        w.Line($"auto&& {subject} = {Expr(sw.Expression)};");
        SwitchSectionSyntax? fallback = null;
        bool first = true;
        foreach (var section in sw.Sections)
        {
            var conditions = new List<string>();
            foreach (var label in section.Labels)
            {
                switch (label)
                {
                    case DefaultSwitchLabelSyntax:
                        fallback = section;
                        break;
                    case CaseSwitchLabelSyntax c:
                        conditions.Add($"({subject} == {ExprAs(c.Value)})");
                        break;
                    case CasePatternSwitchLabelSyntax cp:
                        var condition = Pattern(cp.Pattern, subject);
                        conditions.Add(cp.WhenClause is { } when ? $"(({condition}) && ({Expr(when.Condition)}))" : condition);
                        break;
                }
            }
            if (conditions.Count == 0)
                continue;   // only `default:` — emitted last
            w.OpenBrace($"{(first ? "if" : "else if")} ({string.Join(" || ", conditions)})");
            foreach (var s in section.Statements)
                Statement(w, s);
            w.CloseBrace();
            first = false;
        }
        if (fallback is not null)
        {
            w.OpenBrace(first ? "" : "else");
            foreach (var s in fallback.Statements)
                Statement(w, s);
            w.CloseBrace();
        }
        w.CloseBrace();
        w.CloseBrace();
    }
}
