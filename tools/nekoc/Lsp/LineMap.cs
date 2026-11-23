namespace Nekotia.Compiler.Lsp;

/// <summary>
/// Column mapping between a .neko file and its desugared C#. The desugarer keeps lines; inside a changed
/// line the two texts are aligned token by token (identifiers, numbers, single punctuation), so an
/// identifier always maps onto the same identifier: <c>for i in 0..n {</c> ↔ <c>for (int i = 0; i &lt; n; i++) {</c>.
/// </summary>
public sealed class LineMap
{
    readonly string[] _original;
    readonly string[] _desugared;
    readonly Dictionary<int, (int[] toDesugared, int[] toOriginal)> _changed = [];

    public LineMap(string original, string desugared)
    {
        _original = original.Split('\n');
        _desugared = desugared.Split('\n');
        int n = Math.Min(_original.Length, _desugared.Length);
        for (int i = 0; i < n; i++)
            if (_original[i] != _desugared[i])
                _changed[i] = Align(_original[i], _desugared[i]);
    }

    public bool IsIdentity => _changed.Count == 0;

    public int ToDesugared(int line, int column) => Map(line, column, true);
    public int ToOriginal(int line, int column) => Map(line, column, false);

    /// <summary>Whether the desugared line differs (columns there are approximate outside tokens).</summary>
    public bool LineChanged(int line) => _changed.ContainsKey(line);

    int Map(int line, int column, bool toDesugared)
    {
        if (!_changed.TryGetValue(line, out var m))
            return column;
        var table = toDesugared ? m.toDesugared : m.toOriginal;
        return table[Math.Clamp(column, 0, table.Length - 1)];
    }

    readonly record struct Token(int Start, int Length, string Text);

    static List<Token> Tokens(string s)
    {
        var list = new List<Token>();
        int i = 0;
        while (i < s.Length)
        {
            char c = s[i];
            if (char.IsWhiteSpace(c))
            {
                i++;
                continue;
            }
            int start = i;
            if (char.IsLetterOrDigit(c) || c == '_')
                while (i < s.Length && (char.IsLetterOrDigit(s[i]) || s[i] == '_'))
                    i++;
            else
                i++;
            list.Add(new Token(start, i - start, s[start..i]));
        }
        return list;
    }

    /// <summary>LCS over tokens; matched tokens map character by character, the rest to the nearest match before.</summary>
    static (int[], int[]) Align(string a, string b)
    {
        var ta = Tokens(a);
        var tb = Tokens(b);
        var lcs = new int[ta.Count + 1, tb.Count + 1];
        for (int i = ta.Count - 1; i >= 0; i--)
            for (int j = tb.Count - 1; j >= 0; j--)
                lcs[i, j] = ta[i].Text == tb[j].Text ? lcs[i + 1, j + 1] + 1 : Math.Max(lcs[i + 1, j], lcs[i, j + 1]);

        var aToB = new int[a.Length + 1];
        var bToA = new int[b.Length + 1];
        Array.Fill(aToB, -1);
        Array.Fill(bToA, -1);
        for (int i = 0, j = 0; i < ta.Count && j < tb.Count;)
        {
            if (ta[i].Text == tb[j].Text)
            {
                for (int k = 0; k <= ta[i].Length; k++)   // including the position right after the token
                {
                    aToB[ta[i].Start + k] = tb[j].Start + k;
                    bToA[tb[j].Start + k] = ta[i].Start + k;
                }
                i++;
                j++;
            }
            else if (lcs[i + 1, j] >= lcs[i, j + 1])
                i++;
            else
                j++;
        }
        Fill(aToB, b.Length);
        Fill(bToA, a.Length);
        return (aToB, bToA);
    }

    static void Fill(int[] table, int otherLength)
    {
        int last = 0;
        for (int i = 0; i < table.Length; i++)
        {
            if (table[i] >= 0)
                last = table[i];
            else
                table[i] = Math.Min(last, otherLength);
        }
    }
}
