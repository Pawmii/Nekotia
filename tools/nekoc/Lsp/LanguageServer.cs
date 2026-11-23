using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Nekotia.Compiler.Lsp;

/// <summary>
/// <c>nekoc lsp</c>: a Language Server for .cs and .neko files of Nekotia projects (stdio). The .neko sugar is
/// desugared and every position goes through <see cref="LineMap"/>, so completion, hover, go to definition,
/// references, rename and diagnostics work in .neko as in plain C#. Diagnostics include nekoc's own checks
/// (the supported C# subset). Go to definition on a <c>[Native]</c> member also lands in the C++ runtime.
/// </summary>
public sealed partial class LanguageServer
{
    readonly JsonRpc _rpc;
    readonly object _gate = new();
    Workspace? _ws;
    Timer? _checkTimer;
    readonly HashSet<Project> _dirty = [];
    readonly Dictionary<Project, HashSet<string>> _published = [];
    bool _shutdown;

    public LanguageServer(Stream input, Stream output) => _rpc = new JsonRpc(input, output);

    public int Run()
    {
        while (true)
        {
            var message = _rpc.Read();
            if (message is null)
                return _shutdown ? 0 : 1;
            var method = (string?)message["method"];
            var id = message["id"];
            var @params = message["params"];
            if (method is null)
                continue;   // a response to something we never ask
            if (method == "exit")
                return _shutdown ? 0 : 1;
            try
            {
                JsonNode? result;
                lock (_gate)
                    result = Handle(method, @params);
                if (id is not null)
                    _rpc.Respond(id, result);
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"nekoc lsp: {method}: {e}");
                if (id is not null)
                    _rpc.RespondError(id, -32603, e.Message);
            }
        }
    }

    JsonNode? Handle(string method, JsonNode? p)
    {
        switch (method)
        {
            case "initialize": return Initialize(p!);
            case "initialized": return null;
            case "shutdown": _shutdown = true; return null;
            case "textDocument/didOpen":
            {
                var path = PathOf(p!["textDocument"]!["uri"]!);
                _ws!.SetText(path, (string)p["textDocument"]!["text"]!);
                Touched(path);
                return null;
            }
            case "textDocument/didChange":
            {
                var path = PathOf(p!["textDocument"]!["uri"]!);
                var changes = p["contentChanges"]!.AsArray();
                if (changes.Count > 0)
                    _ws!.SetText(path, (string)changes[^1]!["text"]!);   // full sync
                Touched(path);
                return null;
            }
            case "textDocument/didClose":
            {
                var path = PathOf(p!["textDocument"]!["uri"]!);
                _ws!.Close(path);
                Touched(path);
                return null;
            }
            case "textDocument/didSave": return null;
            case "workspace/didChangeWatchedFiles":
                foreach (var change in p!["changes"]!.AsArray())
                {
                    var path = PathOf(change!["uri"]!);
                    _ws!.FileChanged(path);
                    Touched(path);
                }
                return null;
            case "textDocument/completion": return Completion(p!);
            case "textDocument/hover": return Hover(p!);
            case "textDocument/definition": return Definition(p!);
            case "textDocument/references": return References(p!);
            case "textDocument/prepareRename": return PrepareRename(p!);
            case "textDocument/rename": return Rename(p!);
            case "textDocument/documentSymbol": return DocumentSymbols(p!);
            case "workspace/symbol": return WorkspaceSymbols(p!);
            case "textDocument/signatureHelp": return SignatureHelp(p!);
            case "textDocument/semanticTokens/full": return SemanticTokens(p!);
            case "textDocument/codeLens": return CodeLens(p!);
            default:
                return null;   // unknown notifications are ignored; unknown requests answer null
        }
    }

    // ---------------------------------------------------------------- lifecycle

    JsonNode Initialize(JsonNode p)
    {
        string? root = null;
        if (p["rootUri"] is JsonValue uri && (string?)uri is { } u)
            root = PathOf(u);
        else if ((string?)p["rootPath"] is { } rp)
            root = rp;
        root ??= Directory.GetCurrentDirectory();
        _ws = new Workspace(Path.GetFullPath(root));
        Console.Error.WriteLine($"nekoc lsp: {_ws.Projects.Count} project(s) under {_ws.Root}");

        var legend = new JsonObject
        {
            ["tokenTypes"] = new JsonArray(TokenTypes.Select(t => (JsonNode)t).ToArray()),
            ["tokenModifiers"] = new JsonArray(TokenModifiers.Select(t => (JsonNode)t).ToArray()),
        };
        return new JsonObject
        {
            ["capabilities"] = new JsonObject
            {
                ["textDocumentSync"] = new JsonObject { ["openClose"] = true, ["change"] = 1, ["save"] = new JsonObject { ["includeText"] = false } },
                ["completionProvider"] = new JsonObject { ["triggerCharacters"] = new JsonArray(".", ">") },
                ["hoverProvider"] = true,
                ["definitionProvider"] = true,
                ["referencesProvider"] = true,
                ["renameProvider"] = new JsonObject { ["prepareProvider"] = true },
                ["documentSymbolProvider"] = true,
                ["workspaceSymbolProvider"] = true,
                ["signatureHelpProvider"] = new JsonObject { ["triggerCharacters"] = new JsonArray("(", ",") },
                ["semanticTokensProvider"] = new JsonObject { ["legend"] = legend, ["full"] = true },
                ["codeLensProvider"] = new JsonObject { ["resolveProvider"] = false },
            },
            ["serverInfo"] = new JsonObject { ["name"] = "nekoc", ["version"] = typeof(Driver).Assembly.GetName().Version?.ToString(3) },
        };
    }

    // ---------------------------------------------------------------- diagnostics

    void Touched(string path)
    {
        foreach (var p in _ws!.LoadedProjects)
            if (p.Files.Contains(path))
                _dirty.Add(p);
        if (_ws.ProjectFor(path) is { } own)
            _dirty.Add(own);
        _checkTimer ??= new Timer(_ => CheckDirty());
        _checkTimer.Change(350, Timeout.Infinite);
    }

    /// <summary>Roslyn + nekoc diagnostics for the changed projects (off the message loop, on a snapshot).</summary>
    void CheckDirty()
    {
        List<(Project project, CSharpCompilation compilation, int version, Dictionary<string, Document> docs)> work;
        lock (_gate)
        {
            work = _dirty.Where(p => p.Loaded).Select(p => (p, p.Compilation!, p.Version, new Dictionary<string, Document>(p.Documents))).ToList();
            _dirty.Clear();
        }
        foreach (var (project, compilation, version, docs) in work)
        {
            var byFile = new Dictionary<string, JsonArray>();
            void Add(string? file, JsonObject d)
            {
                if (file is null)
                    return;
                if (!byFile.TryGetValue(file, out var list))
                    byFile[file] = list = [];
                list.Add(d);
            }

            bool errors = false;
            foreach (var d in compilation.GetDiagnostics())
            {
                if (d.Severity < DiagnosticSeverity.Warning || !d.Location.IsInSource)
                    continue;
                errors |= d.Severity == DiagnosticSeverity.Error;
                var path = d.Location.SourceTree!.FilePath;
                if (!docs.TryGetValue(path, out var doc))
                    continue;
                Add(path, Diagnostic(doc.ToRange(d.Location.SourceSpan), d.Severity == DiagnosticSeverity.Error ? 1 : 2, d.Id, d.GetMessage()));
            }
            if (!errors)
            {
                // nekoc's own checks (the C# subset, NEKOxxxx) run on a compilation without Roslyn errors only.
                var sink = new DiagnosticSink();
                try
                {
                    new CppEmitter(compilation, sink, project.Config).Emit();
                }
                catch (Exception e)
                {
                    Console.Error.WriteLine($"nekoc lsp: emitter: {e.Message}");
                }
                foreach (var d in sink.Items)
                {
                    if (d.File is null || !docs.TryGetValue(d.File, out var doc))
                        continue;
                    var start = doc.Map.ToOriginal(d.Line - 1, d.Column - 1);
                    var end = doc.Map.ToOriginal(d.EndLine - 1, d.EndColumn - 1);
                    var severity = d.Severity switch { Severity.Error => 1, Severity.Warning => 2, _ => 3 };
                    Add(d.File, Diagnostic((d.Line - 1, start, d.EndLine - 1, end), severity, d.Code, d.Message));
                }
            }

            lock (_gate)
            {
                if (project.Version != version)
                    continue;   // changed meanwhile: a newer check is scheduled
                _published.TryGetValue(project, out var before);
                var now = new HashSet<string>();
                foreach (var (file, list) in byFile)
                {
                    if (_ws!.ProjectFor(file) != project)
                        continue;   // a shared SDK file reports from its own project
                    Publish(file, list);
                    now.Add(file);
                }
                foreach (var file in before ?? [])
                    if (!now.Contains(file))
                        Publish(file, []);
                _published[project] = now;
            }
        }
    }

    void Publish(string file, JsonArray list) =>
        _rpc.Notify("textDocument/publishDiagnostics", new JsonObject { ["uri"] = UriOf(file), ["diagnostics"] = list });

    static JsonObject Diagnostic((int, int, int, int) range, int severity, string code, string message) => new()
    {
        ["range"] = Range(range),
        ["severity"] = severity,
        ["code"] = code,
        ["source"] = code.StartsWith("NEKO") ? "nekoc" : "roslyn",
        ["message"] = message,
    };

    // ---------------------------------------------------------------- positions

    sealed record At(Project Project, Document Doc, SemanticModel Model, SyntaxNode Root, int Offset);

    At? Locate(JsonNode p)
    {
        var path = PathOf(p["textDocument"]!["uri"]!);
        var project = _ws!.ProjectFor(path);
        if (project is null || !project.Documents.TryGetValue(path, out var doc))
            return null;
        var pos = p["position"]!;
        int offset = doc.ToOffset((int)pos["line"]!, (int)pos["character"]!);
        var model = project.Compilation!.GetSemanticModel(doc.Tree);
        return new At(project, doc, model, doc.Tree.GetRoot(), offset);
    }

    static string PathOf(JsonNode uri) => PathOf((string)uri!);
    static string PathOf(string uri) => Path.GetFullPath(new Uri(uri).LocalPath);
    static string UriOf(string path) => new Uri(path).AbsoluteUri;

    static JsonObject Range((int line, int col, int endLine, int endCol) r) => new()
    {
        ["start"] = new JsonObject { ["line"] = r.line, ["character"] = r.col },
        ["end"] = new JsonObject { ["line"] = r.endLine, ["character"] = r.endCol },
    };

    static JsonObject Location(string path, (int, int, int, int) range) => new() { ["uri"] = UriOf(path), ["range"] = Range(range) };

    /// <summary>The identifier (or predefined type keyword) under or right before the cursor.</summary>
    static SyntaxToken TokenAt(SyntaxNode root, int offset)
    {
        var token = root.FindToken(Math.Min(offset, root.FullSpan.End - 1 < 0 ? 0 : root.FullSpan.End - 1));
        if (IsNameToken(token) && token.Span.Contains(offset))
            return token;
        if (offset > 0)
        {
            var before = root.FindToken(offset - 1);
            if (IsNameToken(before) && before.Span.End == offset)
                return before;
        }
        return token;
    }

    static bool IsNameToken(SyntaxToken t) =>
        t.IsKind(SyntaxKind.IdentifierToken) || SyntaxFacts.IsPredefinedType(t.Kind()) || t.IsKind(SyntaxKind.ThisKeyword) || t.IsKind(SyntaxKind.BaseKeyword);

    /// <summary>What the token names: a reference (bound symbol, or the best candidate) or a declaration.</summary>
    static ISymbol? SymbolOf(SemanticModel model, SyntaxToken token, out bool declaration)
    {
        declaration = false;
        if (!IsNameToken(token) || token.Parent is null)
            return null;
        var parent = token.Parent;
        var info = model.GetSymbolInfo(parent);
        if ((info.Symbol ?? info.CandidateSymbols.FirstOrDefault()) is { } s)
            return s;
        if (parent is IdentifierNameSyntax { Parent: { } outer } && model.GetSymbolInfo(outer) is { Symbol: { } so })
            return so;
        if (parent is IdentifierNameSyntax ins && model.GetTypeInfo(ins).Type is { TypeKind: not TypeKind.Error } t)
            return t;
        var declared = model.GetDeclaredSymbol(parent);
        if (declared is not null)
        {
            declaration = true;
            return declared;
        }
        return null;
    }

    // ---------------------------------------------------------------- hover

    static readonly SymbolDisplayFormat Signature = new(
        globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
        memberOptions: SymbolDisplayMemberOptions.IncludeParameters | SymbolDisplayMemberOptions.IncludeType |
                       SymbolDisplayMemberOptions.IncludeContainingType | SymbolDisplayMemberOptions.IncludeModifiers |
                       SymbolDisplayMemberOptions.IncludeConstantValue | SymbolDisplayMemberOptions.IncludeRef,
        kindOptions: SymbolDisplayKindOptions.IncludeMemberKeyword | SymbolDisplayKindOptions.IncludeTypeKeyword | SymbolDisplayKindOptions.IncludeNamespaceKeyword,
        parameterOptions: SymbolDisplayParameterOptions.IncludeName | SymbolDisplayParameterOptions.IncludeType |
                          SymbolDisplayParameterOptions.IncludeParamsRefOut | SymbolDisplayParameterOptions.IncludeDefaultValue,
        propertyStyle: SymbolDisplayPropertyStyle.ShowReadWriteDescriptor,
        localOptions: SymbolDisplayLocalOptions.IncludeType | SymbolDisplayLocalOptions.IncludeConstantValue,
        miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes | SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers);

    JsonNode? Hover(JsonNode p)
    {
        if (Locate(p) is not { } at)
            return null;
        var token = TokenAt(at.Root, at.Offset);
        var symbol = SymbolOf(at.Model, token, out _);
        if (symbol is null)
            return null;
        var md = new StringBuilder();
        var prefix = symbol switch
        {
            ILocalSymbol => "(local) ",
            IParameterSymbol => "(parameter) ",
            IFieldSymbol { ContainingType.TypeKind: TypeKind.Enum } => "(enum value) ",
            IFieldSymbol => "(field) ",
            _ => "",
        };
        md.Append("```csharp\n").Append(prefix).Append(symbol.ToDisplayString(Signature)).Append("\n```\n");
        if (Summary(symbol) is { Length: > 0 } summary)
            md.Append('\n').Append(summary).Append('\n');
        if (NativeTarget(symbol) is { } native)
            md.Append("\nC++: `").Append(native).Append("` — implemented in the runtime (F12 goes there)\n");
        if (Symbols.Attr(symbol, "Syscall") is { ConstructorArguments: [{ Value: int number }, ..] })
            md.Append("\nSystem call ").Append(number).Append(" — ring-3 programs reach it through `syscall`\n");
        if (symbol is IFieldSymbol { IsConst: true, ConstantValue: { } value } && value is int or uint or long or ulong or ushort or byte)
            md.Append($"\n= 0x{Convert.ToUInt64(value):X}\n");
        return new JsonObject
        {
            ["contents"] = new JsonObject { ["kind"] = "markdown", ["value"] = md.ToString() },
            ["range"] = Range(at.Doc.ToRange(token.Span)),
        };
    }

    /// <summary>The XML doc summary as plain text (with <c>&lt;c&gt;</c> as code).</summary>
    static string Summary(ISymbol symbol)
    {
        var xml = (symbol is IMethodSymbol { PartialImplementationPart: { } impl } ? impl : symbol).GetDocumentationCommentXml();
        if (string.IsNullOrWhiteSpace(xml))
            return "";
        try
        {
            var summary = XElement.Parse(xml).Element("summary");
            if (summary is null)
                return "";
            var sb = new StringBuilder();
            foreach (var node in summary.Nodes())
            {
                switch (node)
                {
                    case XText t: sb.Append(t.Value); break;
                    case XElement { Name.LocalName: "c" } c: sb.Append('`').Append(c.Value).Append('`'); break;
                    case XElement e when e.Attribute("cref") is { } cref: sb.Append('`').Append(cref.Value.Split(':').Last()).Append('`'); break;
                    case XElement e when e.Attribute("name") is { } name: sb.Append('`').Append(name.Value).Append('`'); break;
                    case XElement e: sb.Append(e.Value); break;
                }
            }
            return string.Join(' ', sb.ToString().Split((char[])['\n', '\r'], StringSplitOptions.RemoveEmptyEntries).Select(l => l.Trim())).Trim();
        }
        catch
        {
            return "";
        }
    }

    /// <summary>The C++ name a symbol stands for: [Native("::neko::x")], or a member of a [Native] type.</summary>
    static string? NativeTarget(ISymbol symbol)
    {
        symbol = symbol.OriginalDefinition;
        var name = Symbols.NativeName(symbol);
        if (name is null)
            return null;
        if (!name.StartsWith("::") && symbol.ContainingType is { } type && Symbols.NativeName(type) is { } typeName)
            return typeName + "::" + name;
        return name;
    }

    // ---------------------------------------------------------------- definition

    JsonNode? Definition(JsonNode p)
    {
        if (Locate(p) is not { } at)
            return null;
        var symbol = SymbolOf(at.Model, TokenAt(at.Root, at.Offset), out _);
        if (symbol is null)
            return null;
        if (symbol is IMethodSymbol { ReducedFrom: { } reduced })
            symbol = reduced;
        var result = new JsonArray();
        foreach (var loc in symbol.OriginalDefinition.Locations)
            if (MapLocation(at.Project, loc) is { } l)
                result.Add(l);
        if (NativeTarget(symbol) is { } native)
            foreach (var (file, line, column, length) in FindNative(native, symbol is ITypeSymbol))
                result.Add(Location(file, (line, column, line, column + length)));
        return result;
    }

    JsonObject? MapLocation(Project project, Microsoft.CodeAnalysis.Location loc)
    {
        if (!loc.IsInSource || _ws!.DocumentFor(project, loc.SourceTree!) is not { } doc)
            return null;
        return Location(doc.Path, doc.ToRange(loc.SourceSpan));
    }

    /// <summary>Definitions of a C++ function or type in runtime/ (top-level lines: `void log::write(…) {`, `struct Mutex {`).</summary>
    List<(string file, int line, int column, int length)> FindNative(string qualified, bool isType)
    {
        var parts = qualified.Trim(':').Split("::");
        var name = parts[^1];
        var scope = parts.Length >= 2 ? parts[^2] : null;
        var pattern = isType
            ? new Regex($@"^\s*(?:struct|class|enum(?:\s+class)?|union|using)\s+{Regex.Escape(name)}\b(?!.*;\s*$)")
            : new Regex($@"^(?:[\w:<>,*&\s]+?\s)?[*&]*(?:{Regex.Escape(scope ?? "")}::)?{Regex.Escape(name)}\s*\((?!.*;\s*(//.*)?$)");
        var found = new List<(string, int, int, int, bool)>();
        var runtime = Path.Combine(_ws!.Root, "runtime");
        if (!Directory.Exists(runtime))
            return [];
        foreach (var file in Directory.EnumerateFiles(runtime, "*.*", SearchOption.AllDirectories))
        {
            if (!(file.EndsWith(".cpp") || file.EndsWith(".hpp") || file.EndsWith(".h")) || file.Contains("third_party"))
                continue;
            string[] lines;
            try { lines = File.ReadAllLines(file); }
            catch { continue; }
            bool inScope = scope is null || lines.Any(l => l.Contains($"namespace {scope}") || l.Contains($"::{scope} ") || l.Contains($"::{scope} {{") ||
                                                           l.Contains($"struct {scope}") || l.Contains($"class {scope}"));
            for (int i = 0; i < lines.Length; i++)
            {
                var line = lines[i];
                if (line.Length == 0 || line.TrimStart().StartsWith("//") || line.TrimStart().StartsWith("#") || line.TrimStart().StartsWith("return"))
                    continue;
                if (!isType && char.IsWhiteSpace(line[0]) && !file.EndsWith(".hpp"))
                    continue;   // calls inside function bodies
                var m = pattern.Match(line);
                if (!m.Success)
                    continue;
                int col = line.IndexOf(name, m.Index, StringComparison.Ordinal);
                bool qualifiedHit = scope is not null && line.Contains(scope + "::" + name);
                found.Add((file, i, col < 0 ? 0 : col, name.Length, inScope || qualifiedHit));
            }
        }
        var preferred = found.Where(f => f.Item5).ToList();
        var chosen = preferred.Count > 0 ? preferred : found;
        // A .cpp definition beats a header declaration.
        var defs = chosen.Where(f => f.Item1.EndsWith(".cpp")).ToList();
        if (defs.Count == 0 || isType)
            defs = chosen;
        return defs.Take(6).Select(f => (f.Item1, f.Item2, f.Item3, f.Item4)).ToList();
    }

    // ---------------------------------------------------------------- references and rename

    /// <summary>Every place (declarations and uses) in the loaded projects that names the symbol.</summary>
    List<(Document doc, TextSpan span)> FindReferences(Project origin, ISymbol symbol, bool includeDeclarations)
    {
        symbol = symbol is IMethodSymbol { ReducedFrom: { } r } ? r : symbol;
        symbol = symbol.OriginalDefinition;
        string? docId = symbol is ILocalSymbol or IParameterSymbol or IRangeVariableSymbol or ILabelSymbol
            ? null : DocumentationCommentId.CreateDeclarationId(symbol);
        var name = symbol.Name;
        var seen = new HashSet<(string, int)>();
        var result = new List<(Document, TextSpan)>();

        foreach (var project in _ws!.LoadedProjects)
        {
            ISymbol? target = project == origin ? symbol
                : docId is null ? null : DocumentationCommentId.GetFirstSymbolForDeclarationId(docId, project.Compilation!);
            if (target is null)
                continue;
            foreach (var doc in project.Documents.Values)
            {
                var root = doc.Tree.GetRoot();
                SemanticModel? model = null;
                foreach (var token in root.DescendantTokens())
                {
                    if (!token.IsKind(SyntaxKind.IdentifierToken) || token.ValueText != name)
                        continue;
                    model ??= project.Compilation!.GetSemanticModel(doc.Tree);
                    var found = SymbolOf(model, token, out bool declaration);
                    if (found is IMethodSymbol { ReducedFrom: { } rf })
                        found = rf;
                    if (found is IMethodSymbol { MethodKind: MethodKind.Constructor or MethodKind.Destructor or MethodKind.StaticConstructor } ctor && target is INamedTypeSymbol)
                        found = ctor.ContainingType;
                    if (found is null || !SymbolEqualityComparer.Default.Equals(found.OriginalDefinition, target))
                        continue;
                    if (declaration && !includeDeclarations)
                        continue;
                    if (seen.Add((doc.Path, token.SpanStart)))
                        result.Add((doc, token.Span));
                }
            }
        }
        return result;
    }

    JsonNode? References(JsonNode p)
    {
        if (Locate(p) is not { } at)
            return null;
        var symbol = SymbolOf(at.Model, TokenAt(at.Root, at.Offset), out _);
        if (symbol is null)
            return null;
        bool includeDeclaration = (bool?)p["context"]?["includeDeclaration"] ?? true;
        LoadAllProjectsFor(symbol);
        var result = new JsonArray();
        foreach (var (doc, span) in FindReferences(at.Project, symbol, includeDeclaration))
            result.Add(Location(doc.Path, doc.ToRange(span)));
        return result;
    }

    /// <summary>SDK symbols are used by many programs: load every project that has the declaring file.</summary>
    void LoadAllProjectsFor(ISymbol symbol)
    {
        foreach (var loc in symbol.Locations)
            if (loc.IsInSource && loc.SourceTree!.FilePath is { } file)
                foreach (var project in _ws!.Projects)
                    if (project.Files.Contains(file))
                        _ws.Load(project);
    }

    JsonNode? PrepareRename(JsonNode p)
    {
        if (Locate(p) is not { } at)
            return null;
        var token = TokenAt(at.Root, at.Offset);
        var symbol = SymbolOf(at.Model, token, out _);
        if (symbol is null || !token.IsKind(SyntaxKind.IdentifierToken))
            throw new InvalidOperationException("nothing to rename here");
        if (symbol.Locations.Any(l => !l.IsInSource))
            throw new InvalidOperationException($"'{symbol.Name}' is not declared in this workspace");
        return new JsonObject { ["range"] = Range(at.Doc.ToRange(token.Span)), ["placeholder"] = token.ValueText };
    }

    JsonNode? Rename(JsonNode p)
    {
        if (Locate(p) is not { } at)
            return null;
        var newName = (string)p["newName"]!;
        if (!SyntaxFacts.IsValidIdentifier(newName) || SyntaxFacts.GetKeywordKind(newName) != SyntaxKind.None)
            throw new InvalidOperationException($"'{newName}' is not a valid C# identifier");
        var token = TokenAt(at.Root, at.Offset);
        var symbol = SymbolOf(at.Model, token, out _) ?? throw new InvalidOperationException("nothing to rename here");
        if (symbol.Locations.Any(l => !l.IsInSource))
            throw new InvalidOperationException($"'{symbol.Name}' is not declared in this workspace");
        LoadAllProjectsFor(symbol);

        var changes = new JsonObject();
        foreach (var group in FindReferences(at.Project, symbol, includeDeclarations: true).GroupBy(r => r.doc.Path))
        {
            var edits = new JsonArray();
            foreach (var (doc, span) in group)
            {
                var range = doc.ToRange(span);
                var lines = doc.Text.Split('\n');
                // Only where the editor text really has the old name (sugar may have produced it).
                if (range.Item1 >= lines.Length || range.Item1 != range.Item3 || range.Item4 > lines[range.Item1].Length ||
                    lines[range.Item1][range.Item2..range.Item4] != symbol.Name)
                    continue;
                edits.Add(new JsonObject { ["range"] = Range(range), ["newText"] = newName });
            }
            if (edits.Count > 0)
                changes[UriOf(group.Key)] = edits;
        }
        return new JsonObject { ["changes"] = changes };
    }
}
