namespace Nekotia.Compiler.Sugar;

/// <summary>
/// Marks which characters of a C# source are "code" (as opposed to comments and string/char literals),
/// so that sugar rewrites never touch text inside literals or comments.
/// </summary>
public sealed class CodeMask
{
    readonly bool[] _code;
    readonly int[] _literalEnd; // for an opening quote: index of the closing quote, otherwise -1

    CodeMask(int length)
    {
        _code = new bool[length];
        _literalEnd = new int[length];
        Array.Fill(_literalEnd, -1);
    }

    public bool IsCode(int index) => index >= 0 && index < _code.Length && _code[index];

    public int LiteralEnd(int quoteIndex) => quoteIndex >= 0 && quoteIndex < _literalEnd.Length ? _literalEnd[quoteIndex] : -1;

    public int MatchingParen(string text, int open)
    {
        int depth = 0;
        for (int i = open; i < text.Length; i++)
        {
            if (!_code[i])
                continue;
            if (text[i] == '(')
                depth++;
            else if (text[i] == ')' && --depth == 0)
                return i;
        }
        return -1;
    }

    public static CodeMask Compute(string s)
    {
        var mask = new CodeMask(s.Length);
        int i = 0;
        while (i < s.Length)
        {
            char c = s[i];
            char next = i + 1 < s.Length ? s[i + 1] : '\0';

            if (c == '/' && next == '/')
            {
                while (i < s.Length && s[i] != '\n')
                    i++;
                continue;
            }
            if (c == '/' && next == '*')
            {
                int end = s.IndexOf("*/", i + 2, StringComparison.Ordinal);
                i = end < 0 ? s.Length : end + 2;
                continue;
            }

            // String prefixes: $ @ $@ @$ $$...
            int p = i;
            bool verbatim = false;
            while (p < s.Length && (s[p] == '$' || s[p] == '@'))
            {
                verbatim |= s[p] == '@';
                p++;
            }
            if (p < s.Length && s[p] == '"' && (p > i || c == '"'))
            {
                int quotes = 0;
                while (p + quotes < s.Length && s[p + quotes] == '"')
                    quotes++;

                int end;
                if (quotes >= 3)
                {
                    var fence = new string('"', quotes);
                    int f = s.IndexOf(fence, p + quotes, StringComparison.Ordinal);
                    end = f < 0 ? s.Length - 1 : f + quotes - 1;
                }
                else if (quotes == 2)
                {
                    end = p + 1; // empty string ""
                }
                else
                {
                    end = ScanString(s, p + 1, verbatim);
                }
                mask._literalEnd[p] = end;
                i = end + 1;
                continue;
            }

            if (c == '\'')
            {
                int j = i + 1;
                while (j < s.Length && s[j] != '\'' && s[j] != '\n')
                    j += s[j] == '\\' ? 2 : 1;
                mask._literalEnd[i] = Math.Min(j, s.Length - 1);
                i = j + 1;
                continue;
            }

            if (char.IsLetterOrDigit(c) || c == '_')
            {
                // Identifiers are code; skip them whole so that a '$' or '@' inside never starts a string.
                while (i < s.Length && (char.IsLetterOrDigit(s[i]) || s[i] == '_'))
                    mask._code[i++] = true;
                continue;
            }

            mask._code[i] = true;
            i++;
        }
        return mask;
    }

    static int ScanString(string s, int i, bool verbatim)
    {
        while (i < s.Length)
        {
            if (verbatim)
            {
                if (s[i] == '"')
                {
                    if (i + 1 < s.Length && s[i + 1] == '"')
                    {
                        i += 2;
                        continue;
                    }
                    return i;
                }
                i++;
            }
            else
            {
                if (s[i] == '\\')
                {
                    i += 2;
                    continue;
                }
                if (s[i] == '"' || s[i] == '\n')
                    return i;
                i++;
            }
        }
        return s.Length - 1;
    }
}
