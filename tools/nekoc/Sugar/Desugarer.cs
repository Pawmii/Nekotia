using System.Text;
using System.Text.RegularExpressions;

namespace Nekotia.Compiler.Sugar;

/// <summary>
/// Rewrites a <c>.neko</c> file (C# + Nekotia sugar) into plain C# that Roslyn can parse.
/// Every rewrite keeps the number of lines intact so diagnostics and <c>#line</c> directives
/// still point at the original file.
///
/// Supported sugar (see docs/LANGUAGE.md):
///   asm { ... }            raw Intel-syntax assembly block      → Asm.Emit("...");
///   asm "...";             one-line assembly                    → Asm.Emit("...");
///   defer stmt;            run stmt at scope exit (Zig/Go)      → if (Sugar.Defer) stmt;
///   unless (cond) stmt     inverse if                           → if (!(cond)) stmt
///   loop { ... }           infinite loop                        → while (true) { ... }
///   lock (m) { ... }       Mutex/SpinLock held for the block    → m.Lock(); defer m.Unlock();
///   for [T] i in a..b {    half-open range loop (..= inclusive) → for (T i = a; i < b; i++) {
///   @entry @packed @inline @noreturn @native("x") @align(n) @irq(n) @syscall(n) → [KernelEntry] ...
/// </summary>
public static partial class Desugarer
{
    const string AsmEmit = "global::Nekotia.Asm.Emit";
    const string DeferMarker = "global::Nekotia.Sugar.Defer";

    public static string Desugar(string source)
    {
        var text = source.Replace("\r\n", "\n");
        text = RewriteAsmBlocks(text);
        text = RewriteAsmInline(text);
        text = RewriteLock(text);
        text = RewriteSimple(text, LoopRegex(), "while (true) {", requireStatementStart: true);
        text = RewriteSimple(text, DeferRegex(), $"if ({DeferMarker})", requireStatementStart: true);
        text = RewriteUnless(text);
        text = RewriteForIn(text);
        text = RewriteAttributes(text);
        return text;
    }

    // ---------------------------------------------------------------- asm

    static string RewriteAsmBlocks(string text)
    {
        // One block at a time: assembly text is not C#, so the code mask is only trusted up to the block start.
        while (true)
        {
            var mask = CodeMask.Compute(text);
            Match? found = null;
            foreach (Match m in AsmBlockRegex().Matches(text))
            {
                if (mask.IsCode(m.Index) && IsStatementStart(text, mask, m.Index))
                {
                    found = m;
                    break;
                }
            }
            if (found is null)
                return text;

            int bodyStart = found.Index + found.Length;
            int close = text.IndexOf('}', bodyStart);
            if (close < 0)
                return text; // let Roslyn complain about the unbalanced brace

            var body = text[bodyStart..close];
            var sb = new StringBuilder();
            sb.Append(AsmEmit).Append("(\"\"");
            var lines = body.Split('\n');
            for (int i = 0; i < lines.Length; i++)
            {
                var line = StripAsmComment(lines[i]).Trim();
                if (line.Length > 0)
                    sb.Append(" + \"").Append(EscapeCSharp(line)).Append("\\n\"");
                if (i < lines.Length - 1)
                    sb.Append('\n');
            }
            sb.Append(");");
            text = text[..found.Index] + sb + text[(close + 1)..];
        }
    }

    static string StripAsmComment(string line)
    {
        int slash = line.IndexOf("//", StringComparison.Ordinal);
        int hash = line.IndexOf('#');
        int cut = slash < 0 ? hash : hash < 0 ? slash : Math.Min(slash, hash);
        return cut < 0 ? line : line[..cut];
    }

    static string RewriteAsmInline(string text)
    {
        var mask = CodeMask.Compute(text);
        var sb = new StringBuilder();
        int last = 0;
        foreach (Match m in AsmInlineRegex().Matches(text))
        {
            int quote = m.Index + m.Length - 1;
            if (!mask.IsCode(m.Index) || !IsStatementStart(text, mask, m.Index))
                continue;
            int end = mask.LiteralEnd(quote);
            if (end < 0)
                continue;
            sb.Append(text, last, m.Index - last);
            sb.Append(AsmEmit).Append('(').Append(text, quote, end - quote + 1).Append(')');
            last = end + 1;
        }
        sb.Append(text, last, text.Length - last);
        return sb.ToString();
    }

    // ---------------------------------------------------------------- lock

    /// <summary>
    /// <c>lock (m) { body }</c> → <c>{ (m).Lock(); if (Sugar.Defer) (m).Unlock(); { body } }</c>:
    /// unlocked on every way out of the block. Works with any value that has Lock()/Unlock() (Mutex, SpinLock).
    /// </summary>
    static string RewriteLock(string text)
    {
        while (true)
        {
            var mask = CodeMask.Compute(text);
            Match? found = null;
            foreach (Match m in LockRegex().Matches(text))
            {
                if (mask.IsCode(m.Index) && IsStatementStart(text, mask, m.Index))
                {
                    found = m;
                    break;
                }
            }
            if (found is null)
                return text;

            int open = found.Index + found.Length - 1;
            int close = mask.MatchingParen(text, open);
            if (close < 0)
                return text;
            int brace = close + 1;
            while (brace < text.Length && char.IsWhiteSpace(text[brace]))
                brace++;
            if (brace >= text.Length || text[brace] != '{')
                return text; // only the block form is sugar; Roslyn reports the rest
            int end = mask.MatchingBrace(text, brace);
            if (end < 0)
                return text;

            var target = text[(open + 1)..close].Trim();
            var gap = text[(close + 1)..brace];   // keeps the newline of Allman-style braces
            var head = $"{{ ({target}).Lock(); if ({DeferMarker}) ({target}).Unlock();{gap}{{";
            text = text[..found.Index] + head + text[(brace + 1)..end] + "} }" + text[(end + 1)..];
        }
    }

    // ---------------------------------------------------------------- keywords

    static string RewriteSimple(string text, Regex regex, string replacement, bool requireStatementStart)
    {
        var mask = CodeMask.Compute(text);
        return regex.Replace(text, m =>
            mask.IsCode(m.Index) && (!requireStatementStart || IsStatementStart(text, mask, m.Index)) ? replacement : m.Value);
    }

    static string RewriteUnless(string text)
    {
        while (true)
        {
            var mask = CodeMask.Compute(text);
            Match? found = null;
            foreach (Match m in UnlessRegex().Matches(text))
            {
                if (mask.IsCode(m.Index) && IsStatementStart(text, mask, m.Index))
                {
                    found = m;
                    break;
                }
            }
            if (found is null)
                return text;

            int open = found.Index + found.Length - 1;
            int close = mask.MatchingParen(text, open);
            if (close < 0)
                return text;
            text = text[..found.Index] + "if (!(" + text[(open + 1)..close] + "))" + text[(close + 1)..];
        }
    }

    static string RewriteForIn(string text)
    {
        var mask = CodeMask.Compute(text);
        return ForInRegex().Replace(text, m =>
        {
            if (!mask.IsCode(m.Index))
                return m.Value;
            var type = m.Groups["type"].Success ? m.Groups["type"].Value : "var";
            var name = m.Groups["name"].Value;
            var range = m.Groups["range"].Value;
            int dots = range.IndexOf("..", StringComparison.Ordinal);
            if (dots < 0)
                return m.Value;
            bool inclusive = dots + 2 < range.Length && range[dots + 2] == '=';
            var from = range[..dots].Trim();
            var to = range[(dots + (inclusive ? 3 : 2))..].Trim();
            var cmp = inclusive ? "<=" : "<";
            return $"for ({type} {name} = ({from}); {name} {cmp} ({to}); {name}++) {{";
        });
    }

    // ---------------------------------------------------------------- attributes

    static readonly Dictionary<string, string> AttributeShorthands = new()
    {
        ["entry"] = "global::Nekotia.KernelEntry",
        ["packed"] = "global::Nekotia.Packed",
        ["inline"] = "global::Nekotia.Inline",
        ["noreturn"] = "global::Nekotia.NoReturn",
        ["native"] = "global::Nekotia.Native",
        ["align"] = "global::Nekotia.Align",
        ["irq"] = "global::Nekotia.Irq",
        ["syscall"] = "global::Nekotia.Syscall",
    };

    static string RewriteAttributes(string text)
    {
        while (true)
        {
            var mask = CodeMask.Compute(text);
            Match? found = null;
            foreach (Match m in AttributeRegex().Matches(text))
            {
                if (mask.IsCode(m.Index) && AttributeShorthands.ContainsKey(m.Groups["name"].Value))
                {
                    found = m;
                    break;
                }
            }
            if (found is null)
                return text;

            var full = AttributeShorthands[found.Groups["name"].Value];
            int end = found.Index + found.Length;
            int p = end;
            while (p < text.Length && text[p] is ' ' or '\t')
                p++;
            if (p < text.Length && text[p] == '(')
            {
                int close = mask.MatchingParen(text, p);
                if (close < 0)
                    return text;
                text = text[..found.Index] + "[" + full + text[p..(close + 1)] + "]" + text[(close + 1)..];
            }
            else
            {
                text = text[..found.Index] + "[" + full + "]" + text[end..];
            }
        }
    }

    // ---------------------------------------------------------------- helpers

    /// <summary>True when the keyword at <paramref name="index"/> begins a statement (prev token is ; { } : else or BOF).</summary>
    static bool IsStatementStart(string text, CodeMask mask, int index)
    {
        int i = index - 1;
        while (i >= 0 && (char.IsWhiteSpace(text[i]) || !mask.IsCode(i)))
            i--;
        if (i < 0)
            return true;
        char c = text[i];
        if (c is ';' or '{' or '}' or ':' or ']')
            return true;
        return i >= 3 && text.AsSpan(i - 3, 4).SequenceEqual("else") && (i < 4 || !IsIdent(text[i - 4]));
    }

    static bool IsIdent(char c) => char.IsLetterOrDigit(c) || c == '_';

    static string EscapeCSharp(string s) => s.Replace("\\", "\\\\").Replace("\"", "\\\"");

    [GeneratedRegex(@"\basm\s*\{")]
    private static partial Regex AsmBlockRegex();

    [GeneratedRegex(@"\basm\s+""")]
    private static partial Regex AsmInlineRegex();

    [GeneratedRegex(@"\block\s*\(")]
    private static partial Regex LockRegex();

    [GeneratedRegex(@"\bloop\s*\{")]
    private static partial Regex LoopRegex();

    [GeneratedRegex(@"\bdefer\b")]
    private static partial Regex DeferRegex();

    [GeneratedRegex(@"\bunless\s*\(")]
    private static partial Regex UnlessRegex();

    [GeneratedRegex(@"\bfor\s+(?:(?<type>[A-Za-z_][\w.]*\**)\s+)?(?<name>[A-Za-z_]\w*)\s+in\s+(?<range>[^{\n]+?)\s*\{")]
    private static partial Regex ForInRegex();

    [GeneratedRegex(@"(?<![\w@])@(?<name>[a-z]+)\b")]
    private static partial Regex AttributeRegex();
}
