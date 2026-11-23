using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Nekotia.Compiler.Sugar;

namespace Nekotia.Compiler;

public sealed record DriverResult(bool Success, DiagnosticSink Diagnostics, List<string> WrittenFiles);

/// <summary>Front end: collect sources → desugar .neko → Roslyn compilation → C++ emitter → files.</summary>
public static class Driver
{
    public const string HeaderName = "nekotia.gen.hpp";
    public const string SourceName = "nekotia.gen.cpp";

    public static DriverResult Run(ProjectConfig config, bool emit)
    {
        var diag = new DiagnosticSink();
        var written = new List<string>();

        var parseOptions = new CSharpParseOptions(LanguageVersion.Preview, preprocessorSymbols: config.Defines);
        var trees = new List<SyntaxTree>();
        foreach (var file in config.EnumerateSourceFiles())
        {
            var text = File.ReadAllText(file);
            if (file.EndsWith(".neko", StringComparison.Ordinal))
                text = Desugarer.Desugar(text);
            trees.Add(CSharpSyntaxTree.ParseText(text, parseOptions, path: file, encoding: Encoding.UTF8));
        }

        var options = new CSharpCompilationOptions(
            OutputKind.DynamicallyLinkedLibrary,
            allowUnsafe: true,
            nullableContextOptions: NullableContextOptions.Disable,
            specificDiagnosticOptions: new Dictionary<string, ReportDiagnostic>
            {
                ["CS0626"] = ReportDiagnostic.Suppress, // extern without DllImport: these are [Native] runtime functions
                ["CS0649"] = ReportDiagnostic.Suppress, // field never assigned (filled by the bootloader / hardware)
                ["CS0169"] = ReportDiagnostic.Suppress,
                ["CS0414"] = ReportDiagnostic.Suppress,
                ["CS8500"] = ReportDiagnostic.Suppress,
            });

        var compilation = CSharpCompilation.Create(config.Name, trees, CoreReferences(), options);
        foreach (var d in compilation.GetDiagnostics())
            if (d.Severity >= DiagnosticSeverity.Warning)
                diag.AddRoslyn(d);

        if (diag.HasErrors)
            return new DriverResult(false, diag, written);

        var emitter = new CppEmitter(compilation, diag, config);
        var (header, source) = emitter.Emit();
        if (diag.HasErrors || !emit)
            return new DriverResult(!diag.HasErrors, diag, written);

        var outDir = config.Resolve(config.Output);
        Directory.CreateDirectory(outDir);
        WriteIfChanged(Path.Combine(outDir, HeaderName), header, written);
        WriteIfChanged(Path.Combine(outDir, SourceName), source, written);
        return new DriverResult(true, diag, written);
    }

    static void WriteIfChanged(string path, string content, List<string> written)
    {
        if (!File.Exists(path) || File.ReadAllText(path) != content)
            File.WriteAllText(path, content);
        written.Add(Path.GetRelativePath(Directory.GetCurrentDirectory(), path));
    }

    /// <summary>Only the core library: enough for attributes and primitive types. Any other .NET API is rejected by the emitter.</summary>
    static IEnumerable<MetadataReference> CoreReferences()
    {
        var tpa = (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string ?? "").Split(Path.PathSeparator);
        foreach (var path in tpa)
        {
            var name = Path.GetFileNameWithoutExtension(path);
            if (name is "System.Private.CoreLib" or "System.Runtime")
                yield return MetadataReference.CreateFromFile(path);
        }
    }
}
