using System.Text.Json;

namespace Nekotia.Compiler;

/// <summary>Contents of <c>nekotia.json</c> at the repository root.</summary>
public sealed class ProjectConfig
{
    public string Name { get; set; } = "Nekotia";

    /// <summary>Directories (or single files) with .cs / .neko sources, relative to the config file.</summary>
    public List<string> Sources { get; set; } = [];

    /// <summary>Output directory for the generated C++, relative to the config file.</summary>
    public string Output { get; set; } = "build/gen";

    /// <summary>Emit <c>#line</c> directives so clang errors and gdb point at the C# source.</summary>
    public bool LineDirectives { get; set; } = true;

    public List<string> Defines { get; set; } = ["NEKOTIA"];

    public string RootDirectory { get; private set; } = ".";

    public static ProjectConfig Load(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException($"config '{path}' not found (run nekoc from the repository root or pass --config)");

        var config = JsonSerializer.Deserialize<ProjectConfig>(File.ReadAllText(path), new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
            ReadCommentHandling = JsonCommentHandling.Skip,
            AllowTrailingCommas = true,
        }) ?? throw new InvalidDataException($"config '{path}' is empty");

        config.RootDirectory = Path.GetDirectoryName(Path.GetFullPath(path))!;
        if (config.Sources.Count == 0)
            throw new InvalidDataException($"config '{path}' has no \"sources\"");
        return config;
    }

    public string Resolve(string relative) => Path.GetFullPath(Path.Combine(RootDirectory, relative));

    public IEnumerable<string> EnumerateSourceFiles()
    {
        foreach (var entry in Sources)
        {
            var full = Resolve(entry);
            if (File.Exists(full))
            {
                yield return full;
                continue;
            }
            if (!Directory.Exists(full))
                throw new DirectoryNotFoundException($"source path '{entry}' does not exist");

            var files = Directory.EnumerateFiles(full, "*.*", SearchOption.AllDirectories)
                .Where(f => f.EndsWith(".cs", StringComparison.Ordinal) || f.EndsWith(".neko", StringComparison.Ordinal))
                .Where(f => !IsBuildArtifact(full, f))
                .Order(StringComparer.Ordinal);
            foreach (var f in files)
                yield return f;
        }
    }

    static bool IsBuildArtifact(string root, string file)
    {
        var rel = Path.GetRelativePath(root, file).Replace('\\', '/');
        return rel.StartsWith("bin/", StringComparison.Ordinal) || rel.StartsWith("obj/", StringComparison.Ordinal)
            || rel.Contains("/bin/", StringComparison.Ordinal) || rel.Contains("/obj/", StringComparison.Ordinal);
    }
}
