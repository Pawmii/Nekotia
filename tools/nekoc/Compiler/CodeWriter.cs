using System.Text;

namespace Nekotia.Compiler;

/// <summary>Indentation-aware text builder for generated C++.</summary>
public sealed class CodeWriter
{
    readonly StringBuilder _sb = new();
    int _indent;

    public CodeWriter(int indent = 0) => _indent = indent;

    public bool IsEmpty => _sb.Length == 0;

    public void Append(CodeWriter other) => _sb.Append(other._sb);

    public void Line(string text = "")
    {
        if (text.Length > 0)
            _sb.Append(' ', _indent * 4);
        _sb.Append(text).Append('\n');
    }

    /// <summary>Writes a line at column 0 (preprocessor directives).</summary>
    public void Directive(string text) => _sb.Append(text).Append('\n');

    public void Indent() => _indent++;
    public void Dedent() => _indent--;

    public void OpenBrace(string head = "")
    {
        Line(head.Length == 0 ? "{" : head + " {");
        _indent++;
    }

    public void CloseBrace(string tail = "")
    {
        _indent--;
        Line("}" + tail);
    }

    public override string ToString() => _sb.ToString();
}
