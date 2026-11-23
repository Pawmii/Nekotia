using System.Text.Json.Nodes;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Nekotia.Compiler.Lsp;

// Completion, signature help, outline, workspace symbols, semantic tokens, code lenses.
public sealed partial class LanguageServer
{
    // ---------------------------------------------------------------- completion

    static readonly string[] Keywords =
    [
        "bool", "byte", "sbyte", "char", "short", "ushort", "int", "uint", "long", "ulong", "nint", "nuint", "float", "double",
        "void", "string", "var", "const", "static", "readonly", "unsafe", "fixed", "stackalloc", "sizeof", "default", "null",
        "true", "false", "new", "return", "if", "else", "while", "do", "for", "foreach", "break", "continue", "switch", "case",
        "goto", "try", "catch", "finally", "throw", "when", "is", "not", "and", "or", "out", "ref", "in", "this", "typeof",
        "nameof", "public", "private", "internal", "struct", "class", "enum", "interface", "delegate", "using", "namespace",
        "extern", "volatile", "checked", "unchecked",
    ];

    // .neko sugar (docs/LANGUAGE.md)
    static readonly (string word, string detail)[] Sugar =
    [
        ("defer", "run a statement at scope exit"), ("loop", "infinite loop"), ("unless", "inverse if"),
        ("lock", "hold a Mutex/SpinLock for the block"), ("asm", "inline assembly block (Intel syntax)"),
    ];

    static readonly HashSet<string> ObjectMembers = ["Equals", "GetHashCode", "ToString", "GetType", "ReferenceEquals", "MemberwiseClone", "Finalize"];

    JsonNode? Completion(JsonNode p)
    {
        if (Locate(p) is not { } at)
            return null;
        var model = at.Model;
        int pos = at.Offset;
        var token = at.Root.FindToken(Math.Max(0, pos - 1));
        if (token.IsKind(SyntaxKind.IdentifierToken) && token.Span.End < pos)
            token = default;   // `foo |`: nothing being typed

        // What is left of the dot: `x.`, `x.Na|`, `p->`, `Nekotia.Hal.`
        SyntaxNode? left = null;
        bool pointer = false;
        if (token.IsKind(SyntaxKind.DotToken) || token.IsKind(SyntaxKind.MinusGreaterThanToken))
            left = token.Parent switch
            {
                MemberAccessExpressionSyntax ma => ma.Expression,
                QualifiedNameSyntax q => q.Left,
                _ => null,
            };
        else if (token.IsKind(SyntaxKind.IdentifierToken) && token.Parent is SimpleNameSyntax name)
            left = name.Parent switch
            {
                MemberAccessExpressionSyntax ma when ma.Name == name => ma.Expression,
                QualifiedNameSyntax q when q.Right == name => q.Left,
                _ => null,
            };
        if (left?.Parent is MemberAccessExpressionSyntax { RawKind: (int)SyntaxKind.PointerMemberAccessExpression })
            pointer = true;

        IEnumerable<ISymbol> symbols;
        bool members = left is not null;
        if (left is not null)
        {
            var leftSymbol = model.GetSymbolInfo(left).Symbol;
            var leftType = model.GetTypeInfo(left).Type;
            if (leftSymbol is INamespaceSymbol ns)
                symbols = model.LookupNamespacesAndTypes(pos, ns);
            else if (leftSymbol is INamedTypeSymbol type)   // `Log.`: static members and nested types
                symbols = model.LookupStaticMembers(pos, type).Concat(type.GetTypeMembers());
            else if (leftType is not null)
            {
                if (pointer && leftType is IPointerTypeSymbol ptr)
                    leftType = ptr.PointedAtType;
                symbols = model.LookupSymbols(pos, leftType).Where(s => !s.IsStatic || s is INamedTypeSymbol);
            }
            else
                return new JsonObject { ["isIncomplete"] = false, ["items"] = new JsonArray() };
        }
        else
        {
            symbols = model.LookupSymbols(pos).Where(s => s.Locations.Any(l => l.IsInSource) || s is INamespaceSymbol { IsGlobalNamespace: false });
        }

        var items = new JsonArray();
        var seen = new HashSet<string>();
        foreach (var s in symbols)
        {
            if (s.CanBeReferencedByName is false && s is not INamespaceSymbol || s.Name.Length == 0 || s.IsImplicitlyDeclared && s is not IParameterSymbol)
                continue;
            if (s is IMethodSymbol { MethodKind: not (MethodKind.Ordinary or MethodKind.LocalFunction or MethodKind.ReducedExtension) })
                continue;
            if (ObjectMembers.Contains(s.Name) && s.ContainingType?.SpecialType is SpecialType.System_Object or SpecialType.System_ValueType or SpecialType.System_Enum)
                continue;
            if (s.ContainingNamespace?.ToDisplayString() is "System" or "System.Runtime.CompilerServices" && !members && s is INamedTypeSymbol)
                continue;
            if (!seen.Add(s.Name))
                continue;   // overloads: one entry
            var item = new JsonObject
            {
                ["label"] = s.Name,
                ["kind"] = CompletionKind(s),
                ["detail"] = s.ToDisplayString(Signature),
                ["sortText"] = (s is ILocalSymbol or IParameterSymbol ? "0" : members ? "1" : "2") + s.Name,
            };
            if (members && Summary(s) is { Length: > 0 } doc)
                item["documentation"] = new JsonObject { ["kind"] = "markdown", ["value"] = doc };
            items.Add(item);
        }
        if (!members)
        {
            foreach (var k in Keywords)
                if (seen.Add(k))
                    items.Add(new JsonObject { ["label"] = k, ["kind"] = 14, ["sortText"] = "3" + k });
            if (at.Doc.IsNeko)
                foreach (var (word, detail) in Sugar)
                    if (seen.Add(word))
                        items.Add(new JsonObject { ["label"] = word, ["kind"] = 14, ["detail"] = ".neko: " + detail, ["sortText"] = "3" + word });
        }
        return new JsonObject { ["isIncomplete"] = false, ["items"] = items };
    }

    static int CompletionKind(ISymbol s) => s switch
    {
        INamespaceSymbol => 9,
        INamedTypeSymbol { TypeKind: TypeKind.Enum } => 13,
        INamedTypeSymbol { TypeKind: TypeKind.Struct } => 22,
        INamedTypeSymbol { TypeKind: TypeKind.Interface } => 8,
        INamedTypeSymbol => 7,
        IMethodSymbol { IsExtern: true } => 3,
        IMethodSymbol => 2,
        IPropertySymbol => 10,
        IFieldSymbol { ContainingType.TypeKind: TypeKind.Enum } => 20,
        IFieldSymbol { IsConst: true } => 21,
        IFieldSymbol => 5,
        IEventSymbol => 23,
        ILocalSymbol or IParameterSymbol => 6,
        ITypeParameterSymbol => 25,
        _ => 1,
    };

    // ---------------------------------------------------------------- signature help

    static readonly SymbolDisplayFormat ParameterFormat = new(
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
        parameterOptions: SymbolDisplayParameterOptions.IncludeName | SymbolDisplayParameterOptions.IncludeType |
                          SymbolDisplayParameterOptions.IncludeParamsRefOut | SymbolDisplayParameterOptions.IncludeDefaultValue,
        miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes);

    JsonNode? SignatureHelp(JsonNode p)
    {
        if (Locate(p) is not { } at)
            return null;
        int pos = at.Offset;
        var token = at.Root.FindToken(Math.Max(0, pos - 1));
        ArgumentListSyntax? args = null;
        for (var n = token.Parent; n is not null; n = n.Parent)
        {
            if (n is ArgumentListSyntax a && a.OpenParenToken.Span.End <= pos && (a.CloseParenToken.IsMissing || pos <= a.CloseParenToken.SpanStart))
            {
                args = a;
                break;
            }
            if (n is StatementSyntax or MemberDeclarationSyntax)
                break;
        }
        if (args is null)
            return null;

        IEnumerable<IMethodSymbol> methods = args.Parent switch
        {
            InvocationExpressionSyntax inv => at.Model.GetMemberGroup(inv.Expression).OfType<IMethodSymbol>(),
            BaseObjectCreationExpressionSyntax create => (at.Model.GetTypeInfo(create).Type as INamedTypeSymbol)?.InstanceConstructors ?? [],
            _ => [],
        };
        var list = methods.ToList();
        if (list.Count == 0)
            return null;
        int active = args.Arguments.GetSeparators().Count(s => s.SpanStart < pos);
        var bound = at.Model.GetSymbolInfo(args.Parent!).Symbol as IMethodSymbol;
        int activeSignature = bound is not null ? Math.Max(0, list.FindIndex(m => SymbolEqualityComparer.Default.Equals(m, bound))) :
            Math.Max(0, list.FindIndex(m => m.Parameters.Length > active));

        var signatures = new JsonArray();
        foreach (var m in list)
        {
            var parameters = new JsonArray();
            foreach (var param in m.Parameters)
            {
                var entry = new JsonObject { ["label"] = param.ToDisplayString(ParameterFormat) };
                parameters.Add(entry);
            }
            var signature = new JsonObject
            {
                ["label"] = $"{m.ContainingType?.Name}.{(m.MethodKind == MethodKind.Constructor ? m.ContainingType?.Name : m.Name)}(" +
                            string.Join(", ", m.Parameters.Select(x => x.ToDisplayString(ParameterFormat))) + ")",
                ["parameters"] = parameters,
            };
            if (Summary(m) is { Length: > 0 } doc)
                signature["documentation"] = new JsonObject { ["kind"] = "markdown", ["value"] = doc };
            signatures.Add(signature);
        }
        return new JsonObject { ["signatures"] = signatures, ["activeSignature"] = activeSignature, ["activeParameter"] = active };
    }

    // ---------------------------------------------------------------- outline

    JsonNode? DocumentSymbols(JsonNode p)
    {
        var path = PathOf(p["textDocument"]!["uri"]!);
        if (_ws!.ProjectFor(path) is not { } project || !project.Documents.TryGetValue(path, out var doc))
            return null;
        var result = new JsonArray();
        foreach (var member in ((CompilationUnitSyntax)doc.Tree.GetRoot()).Members)
            foreach (var s in Outline(doc, member))
                result.Add(s);
        return result;
    }

    static IEnumerable<JsonObject> Outline(Document doc, MemberDeclarationSyntax member)
    {
        JsonObject Entry(string name, int kind, SyntaxNode node, SyntaxToken id, string? detail = null, IEnumerable<MemberDeclarationSyntax>? children = null)
        {
            var o = new JsonObject
            {
                ["name"] = name,
                ["kind"] = kind,
                ["range"] = Range(doc.ToRange(node.Span)),
                ["selectionRange"] = Range(doc.ToRange(id.Span)),
            };
            if (detail is not null)
                o["detail"] = detail;
            if (children is not null)
                o["children"] = new JsonArray(children.SelectMany(c => Outline(doc, c)).Select(x => (JsonNode)x).ToArray());
            return o;
        }

        string Params(BaseParameterListSyntax? list) => list is null ? "" : "(" + string.Join(", ", list.Parameters.Select(x => x.Type?.ToString())) + ")";

        switch (member)
        {
            case BaseNamespaceDeclarationSyntax ns:
                foreach (var m in ns.Members)
                    foreach (var o in Outline(doc, m))
                        yield return o;   // file-scoped namespaces are the norm: no extra level
                break;
            case EnumDeclarationSyntax e:
                yield return new JsonObject
                {
                    ["name"] = e.Identifier.ValueText, ["kind"] = 10,
                    ["range"] = Range(doc.ToRange(e.Span)), ["selectionRange"] = Range(doc.ToRange(e.Identifier.Span)),
                    ["children"] = new JsonArray(e.Members.Select(m => (JsonNode)new JsonObject
                    {
                        ["name"] = m.Identifier.ValueText, ["kind"] = 22,
                        ["range"] = Range(doc.ToRange(m.Span)), ["selectionRange"] = Range(doc.ToRange(m.Identifier.Span)),
                    }).ToArray()),
                };
                break;
            case TypeDeclarationSyntax t:
                int kind = t switch { StructDeclarationSyntax => 23, InterfaceDeclarationSyntax => 11, _ => 5 };
                yield return Entry(t.Identifier.ValueText, kind, t, t.Identifier, children: t.Members);
                break;
            case DelegateDeclarationSyntax d:
                yield return Entry(d.Identifier.ValueText, 12, d, d.Identifier, Params(d.ParameterList));
                break;
            case MethodDeclarationSyntax m:
                yield return Entry(m.Identifier.ValueText, 6, m, m.Identifier, Params(m.ParameterList));
                break;
            case ConstructorDeclarationSyntax c:
                yield return Entry(c.Identifier.ValueText, 9, c, c.Identifier, Params(c.ParameterList));
                break;
            case PropertyDeclarationSyntax pr:
                yield return Entry(pr.Identifier.ValueText, 7, pr, pr.Identifier, pr.Type.ToString());
                break;
            case IndexerDeclarationSyntax ix:
                yield return Entry("this[]", 7, ix, ix.ThisKeyword, Params(ix.ParameterList));
                break;
            case OperatorDeclarationSyntax op:
                yield return Entry("operator " + op.OperatorToken.Text, 25, op, op.OperatorToken, Params(op.ParameterList));
                break;
            case ConversionOperatorDeclarationSyntax cv:
                yield return Entry("operator " + cv.Type, 25, cv, cv.ImplicitOrExplicitKeyword, Params(cv.ParameterList));
                break;
            case BaseFieldDeclarationSyntax f:
                bool isConst = f.Modifiers.Any(SyntaxKind.ConstKeyword);
                foreach (var v in f.Declaration.Variables)
                    yield return Entry(v.Identifier.ValueText, isConst ? 14 : 8, v, v.Identifier, f.Declaration.Type.ToString());
                break;
        }
    }

    JsonNode? WorkspaceSymbols(JsonNode p)
    {
        var query = (string?)p["query"] ?? "";
        if (!_ws!.LoadedProjects.Any() && _ws.Projects.FirstOrDefault(x => x.Config.RootDirectory == _ws.Root) is { } root)
            _ws.Load(root);
        var result = new JsonArray();
        var seen = new HashSet<(string, int)>();
        foreach (var project in _ws.LoadedProjects)
        {
            foreach (var s in project.Compilation!.GetSymbolsWithName(n => n.Contains(query, StringComparison.OrdinalIgnoreCase), SymbolFilter.TypeAndMember))
            {
                if (s is IMethodSymbol { MethodKind: not (MethodKind.Ordinary or MethodKind.Constructor) } || s.IsImplicitlyDeclared)
                    continue;
                foreach (var loc in s.Locations)
                {
                    if (!loc.IsInSource || _ws.DocumentFor(project, loc.SourceTree!) is not { } doc || !seen.Add((doc.Path, loc.SourceSpan.Start)))
                        continue;
                    result.Add(new JsonObject
                    {
                        ["name"] = s.Name,
                        ["kind"] = SymbolKind(s),
                        ["containerName"] = s.ContainingType?.ToDisplayString() ?? s.ContainingNamespace?.ToDisplayString(),
                        ["location"] = Location(doc.Path, doc.ToRange(loc.SourceSpan)),
                    });
                    if (result.Count >= 300)
                        return result;
                }
            }
        }
        return result;
    }

    static int SymbolKind(ISymbol s) => s switch
    {
        INamedTypeSymbol { TypeKind: TypeKind.Enum } => 10,
        INamedTypeSymbol { TypeKind: TypeKind.Struct } => 23,
        INamedTypeSymbol { TypeKind: TypeKind.Interface } => 11,
        INamedTypeSymbol => 5,
        IMethodSymbol { MethodKind: MethodKind.Constructor } => 9,
        IMethodSymbol => 6,
        IPropertySymbol => 7,
        IFieldSymbol { ContainingType.TypeKind: TypeKind.Enum } => 22,
        IFieldSymbol { IsConst: true } => 14,
        IFieldSymbol => 8,
        IEventSymbol => 24,
        _ => 13,
    };

    // ---------------------------------------------------------------- semantic tokens

    static readonly string[] TokenTypes =
        ["namespace", "class", "struct", "enum", "interface", "typeParameter", "type", "parameter", "variable", "property", "enumMember", "event", "function", "method"];

    // `native`: implemented in C++ ([Native] / extern) — the extension maps it to its own colour.
    static readonly string[] TokenModifiers = ["declaration", "static", "readonly", "defaultLibrary", "native"];

    JsonNode? SemanticTokens(JsonNode p)
    {
        var path = PathOf(p["textDocument"]!["uri"]!);
        if (_ws!.ProjectFor(path) is not { } project || !project.Documents.TryGetValue(path, out var doc))
            return null;
        var model = project.Compilation!.GetSemanticModel(doc.Tree);
        var lines = doc.Text.Split('\n');
        var tokens = new List<(int line, int col, int length, int type, int mods)>();
        foreach (var token in doc.Tree.GetRoot().DescendantTokens())
        {
            if (!token.IsKind(SyntaxKind.IdentifierToken))
                continue;
            if (token.Parent is IdentifierNameSyntax { IsVar: true } || token.ValueText is "nameof" or "value" && token.Parent is IdentifierNameSyntax ins && model.GetSymbolInfo(ins).Symbol is null)
                continue;
            var symbol = SymbolOf(model, token, out bool declaration);
            if (symbol is null || Classify(symbol) is not { } type)
                continue;
            var (line, col, endLine, endCol) = doc.ToRange(token.Span);
            if (line != endLine || line >= lines.Length || endCol > lines[line].Length || lines[line][col..endCol] != token.ValueText)
                continue;   // produced by sugar: not in the editor text
            int mods = 0;
            if (declaration) mods |= 1;
            if (symbol.IsStatic && symbol is not (INamespaceSymbol or ITypeSymbol)) mods |= 2;
            if (symbol is IFieldSymbol { IsReadOnly: true } or IFieldSymbol { IsConst: true } or ILocalSymbol { IsConst: true }) mods |= 4;
            if (!symbol.Locations.Any(l => l.IsInSource)) mods |= 8;
            if (Symbols.NativeName(symbol) is not null || symbol is IMethodSymbol { IsExtern: true }) mods |= 16;
            tokens.Add((line, col, endCol - col, type, mods));
        }
        tokens.Sort((a, b) => a.line != b.line ? a.line.CompareTo(b.line) : a.col.CompareTo(b.col));

        var data = new JsonArray();
        int prevLine = 0, prevCol = 0;
        foreach (var t in tokens)
        {
            int deltaLine = t.line - prevLine;
            int deltaCol = deltaLine == 0 ? t.col - prevCol : t.col;
            if (deltaLine == 0 && deltaCol < 0)
                continue;   // overlap (cannot happen with identifiers, but be safe)
            data.Add(deltaLine);
            data.Add(deltaCol);
            data.Add(t.length);
            data.Add(t.type);
            data.Add(t.mods);
            prevLine = t.line;
            prevCol = t.col;
        }
        return new JsonObject { ["data"] = data };
    }

    static int? Classify(ISymbol s) => s switch
    {
        INamespaceSymbol => 0,
        INamedTypeSymbol { TypeKind: TypeKind.Struct } => 2,
        INamedTypeSymbol { TypeKind: TypeKind.Enum } => 3,
        INamedTypeSymbol { TypeKind: TypeKind.Interface } => 4,
        INamedTypeSymbol { TypeKind: TypeKind.Delegate } => 6,
        INamedTypeSymbol => 1,
        ITypeParameterSymbol => 5,
        IParameterSymbol => 7,
        ILocalSymbol or IRangeVariableSymbol => 8,
        IFieldSymbol { ContainingType.TypeKind: TypeKind.Enum } => 10,
        IFieldSymbol or IPropertySymbol => 9,
        IEventSymbol => 11,
        IMethodSymbol { MethodKind: MethodKind.LocalFunction } => 12,
        IMethodSymbol { MethodKind: MethodKind.Constructor } m => Classify(m.ContainingType),
        IMethodSymbol => 13,
        _ => null,
    };

    // ---------------------------------------------------------------- code lenses

    /// <summary>"C++" above every method with a body: opens the generated C++ for it (nekotia.showGeneratedCpp).</summary>
    JsonNode? CodeLens(JsonNode p)
    {
        var path = PathOf(p["textDocument"]!["uri"]!);
        if (_ws!.ProjectFor(path) is not { } project || !project.Documents.TryGetValue(path, out var doc))
            return null;
        var result = new JsonArray();
        var uri = UriOf(path);
        foreach (var node in doc.Tree.GetRoot().DescendantNodes())
        {
            if (node is not BaseMethodDeclarationSyntax { Body: not null } and not BaseMethodDeclarationSyntax { ExpressionBody: not null })
                continue;
            var id = node switch
            {
                MethodDeclarationSyntax m => m.Identifier,
                ConstructorDeclarationSyntax c => c.Identifier,
                OperatorDeclarationSyntax o => o.OperatorToken,
                _ => ((BaseMethodDeclarationSyntax)node).ParameterList.OpenParenToken,
            };
            var range = doc.ToRange(id.Span);
            var bodyStart = ((BaseMethodDeclarationSyntax)node).Body?.OpenBraceToken.Span ?? ((BaseMethodDeclarationSyntax)node).ExpressionBody!.Span;
            var bodyLine = doc.ToRange(bodyStart).Item1;
            result.Add(new JsonObject
            {
                ["range"] = Range(range),
                ["command"] = new JsonObject
                {
                    ["title"] = "C++",
                    ["command"] = "nekotia.showGeneratedCpp",
                    ["arguments"] = new JsonArray(uri, bodyLine + 1),
                },
            });
        }
        return result;
    }
}
