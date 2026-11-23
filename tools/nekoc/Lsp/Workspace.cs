using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;
using Nekotia.Compiler.Sugar;

namespace Nekotia.Compiler.Lsp;

/// <summary>One source file as the server sees it: the editor's text, its C# form and the column map.</summary>
public sealed class Document
{
    public required string Path { get; init; }
    public required string Text { get; init; }      // as in the editor (\n line ends)
    public required SyntaxTree Tree { get; init; }   // desugared C#
    public required LineMap Map { get; init; }

    public bool IsNeko => Path.EndsWith(".neko", StringComparison.Ordinal);

    /// <summary>Editor position → offset in the C# tree.</summary>
    public int ToOffset(int line, int character)
    {
        var text = Tree.GetText();
        if (line >= text.Lines.Count)
            return text.Length;
        var l = text.Lines[line];
        int column = Map.ToDesugared(line, character);
        return Math.Min(l.Start + column, l.End);
    }

    /// <summary>C# span → editor range (line, start column, end line, end column).</summary>
    public (int, int, int, int) ToRange(TextSpan span)
    {
        var ls = Tree.GetText().Lines.GetLinePositionSpan(span);
        return (ls.Start.Line, Map.ToOriginal(ls.Start.Line, ls.Start.Character),
                ls.End.Line, Map.ToOriginal(ls.End.Line, ls.End.Character));
    }

    public static Document Create(string path, string text, CSharpParseOptions options)
    {
        text = text.Replace("\r\n", "\n");
        var csharp = path.EndsWith(".neko", StringComparison.Ordinal) ? Desugarer.Desugar(text) : text;
        var tree = CSharpSyntaxTree.ParseText(csharp, options, path: path, encoding: Encoding.UTF8);
        return new Document { Path = path, Text = text, Tree = tree, Map = new LineMap(text, csharp) };
    }
}

/// <summary>A nekotia.json project: its files and the Roslyn compilation, kept up to date with the editor.</summary>
public sealed class Project
{
    public required ProjectConfig Config { get; init; }
    public required HashSet<string> Files { get; init; }
    public Dictionary<string, Document> Documents { get; } = [];
    public CSharpCompilation? Compilation { get; set; }
    public int Version { get; set; }

    public bool Loaded => Compilation is not null;
}

/// <summary>Every nekotia.json under the workspace root; projects are parsed when one of their files is used.</summary>
public sealed class Workspace
{
    readonly List<Project> _projects = [];
    readonly Dictionary<string, string> _open = [];   // editor overlays
    public string Root { get; }

    public Workspace(string root)
    {
        Root = root;
        foreach (var config in Directory.EnumerateFiles(root, "nekotia.json", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
        {
            var rel = System.IO.Path.GetRelativePath(root, config).Replace('\\', '/');
            if (rel.StartsWith("build/") || rel.Contains("/bin/") || rel.Contains("/obj/") || rel.StartsWith("tests/"))
                continue;
            try
            {
                var c = ProjectConfig.Load(config);
                _projects.Add(new Project { Config = c, Files = c.EnumerateSourceFiles().ToHashSet() });
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"nekoc lsp: skipping {rel}: {e.Message}");
            }
        }
    }

    public IReadOnlyList<Project> Projects => _projects;
    public IEnumerable<Project> LoadedProjects => _projects.Where(p => p.Loaded);

    /// <summary>The project a file is edited in: the one whose nekotia.json is closest to it (user/sh/… → sh).</summary>
    public Project? ProjectFor(string path)
    {
        Project? best = null;
        int bestDepth = -1;
        foreach (var p in _projects)
        {
            if (!p.Files.Contains(path))
                continue;
            var root = p.Config.RootDirectory + System.IO.Path.DirectorySeparatorChar;
            int depth = path.StartsWith(root, StringComparison.Ordinal) ? root.Length : 0;
            if (depth > bestDepth)
            {
                best = p;
                bestDepth = depth;
            }
        }
        if (best is not null)
            Load(best);
        return best;
    }

    public void Load(Project p)
    {
        if (p.Loaded)
            return;
        var options = Driver.ParseOptions(p.Config);
        foreach (var file in p.Files)
            p.Documents[file] = Document.Create(file, _open.TryGetValue(file, out var t) ? t : File.ReadAllText(file), options);
        p.Compilation = Driver.CreateCompilation(p.Config, p.Documents.Values.Select(d => d.Tree));
        p.Version++;
    }

    public void SetText(string path, string text)
    {
        _open[path] = text;
        Update(path, text);
    }

    public void Close(string path)
    {
        _open.Remove(path);
        if (File.Exists(path))
            Update(path, File.ReadAllText(path));
    }

    /// <summary>A file changed on disk (saved elsewhere, created, deleted): reread unless the editor owns it.</summary>
    public void FileChanged(string path)
    {
        if (_open.ContainsKey(path))
            return;
        bool exists = File.Exists(path);
        foreach (var p in _projects)
        {
            bool belongs = exists && BelongsTo(p, path);
            if (belongs && !p.Files.Contains(path))
            {
                p.Files.Add(path);
                if (p.Loaded)
                {
                    var doc = Document.Create(path, File.ReadAllText(path), Driver.ParseOptions(p.Config));
                    p.Documents[path] = doc;
                    p.Compilation = p.Compilation!.AddSyntaxTrees(doc.Tree);
                    p.Version++;
                }
            }
            else if (!exists && p.Files.Remove(path) && p.Documents.Remove(path, out var old))
            {
                p.Compilation = p.Compilation!.RemoveSyntaxTrees(old.Tree);
                p.Version++;
            }
        }
        if (exists)
            Update(path, File.ReadAllText(path));
    }

    static bool BelongsTo(Project p, string path)
    {
        if (!(path.EndsWith(".cs") || path.EndsWith(".neko")))
            return false;
        foreach (var entry in p.Config.Sources)
        {
            var full = p.Config.Resolve(entry);
            if (path == full || path.StartsWith(full + System.IO.Path.DirectorySeparatorChar, StringComparison.Ordinal))
                return !path.Contains("/bin/") && !path.Contains("/obj/");
        }
        return false;
    }

    void Update(string path, string text)
    {
        foreach (var p in _projects)
        {
            if (!p.Loaded || !p.Documents.TryGetValue(path, out var old))
                continue;
            if (old.Text == text.Replace("\r\n", "\n"))
                continue;
            var doc = Document.Create(path, text, Driver.ParseOptions(p.Config));
            p.Documents[path] = doc;
            p.Compilation = p.Compilation!.ReplaceSyntaxTree(old.Tree, doc.Tree);
            p.Version++;
        }
    }

    public Document? DocumentFor(Project p, SyntaxTree tree) =>
        tree.FilePath is { } path && p.Documents.TryGetValue(path, out var d) && d.Tree == tree ? d : null;
}
