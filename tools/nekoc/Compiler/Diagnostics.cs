using System.Text.Json;
using Microsoft.CodeAnalysis;

namespace Nekotia.Compiler;

public enum Severity { Error, Warning, Info }

public sealed record NekoDiagnostic(Severity Severity, string Code, string Message, string? File, int Line, int Column, int EndLine, int EndColumn);

/// <summary>Collects Roslyn and nekoc diagnostics. Text output uses the MSBuild format so the VS Code $msCompile matcher understands it.</summary>
public sealed class DiagnosticSink
{
    readonly List<NekoDiagnostic> _items = [];

    public IReadOnlyList<NekoDiagnostic> Items => _items;
    public bool HasErrors => _items.Any(d => d.Severity == Severity.Error);

    public void Error(string code, string message, Location? location) => Add(Severity.Error, code, message, location);
    public void Warning(string code, string message, Location? location) => Add(Severity.Warning, code, message, location);

    public void Error(string code, string message, SyntaxNode node) => Add(Severity.Error, code, message, node.GetLocation());

    public void Add(Severity severity, string code, string message, Location? location)
    {
        if (location is { IsInSource: true })
        {
            var span = location.GetLineSpan();
            _items.Add(new NekoDiagnostic(severity, code, message, span.Path,
                span.StartLinePosition.Line + 1, span.StartLinePosition.Character + 1,
                span.EndLinePosition.Line + 1, span.EndLinePosition.Character + 1));
        }
        else
        {
            _items.Add(new NekoDiagnostic(severity, code, message, null, 0, 0, 0, 0));
        }
    }

    public void AddRoslyn(Diagnostic d)
    {
        var severity = d.Severity switch
        {
            DiagnosticSeverity.Error => Severity.Error,
            DiagnosticSeverity.Warning => Severity.Warning,
            _ => Severity.Info,
        };
        Add(severity, d.Id, d.GetMessage(), d.Location);
    }

    public void PrintText(TextWriter writer)
    {
        foreach (var d in _items)
        {
            var sev = d.Severity.ToString().ToLowerInvariant();
            if (d.File is null)
                writer.WriteLine($"nekoc: {sev} {d.Code}: {d.Message}");
            else
                writer.WriteLine($"{d.File}({d.Line},{d.Column}): {sev} {d.Code}: {d.Message}");
        }
    }

    public string ToJson() => JsonSerializer.Serialize(_items.Select(d => new
    {
        severity = d.Severity.ToString().ToLowerInvariant(),
        code = d.Code,
        message = d.Message,
        file = d.File,
        line = d.Line,
        column = d.Column,
        endLine = d.EndLine,
        endColumn = d.EndColumn,
    }));
}
