using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Nekotia.Compiler;

/// <summary>
/// Turns the checked C# compilation into one C++ header + one C++ translation unit.
///
/// Mapping overview (details in docs/ARCHITECTURE.md):
///   namespace A.B        → namespace A::B
///   static class         → struct with only static members (ctor deleted)
///   struct               → C++ struct, same field order; methods become member functions
///   enum                 → enum class with the same underlying type (+ bitwise operators)
///   [Native("x")]        → not emitted, references are spelled as x (implemented in runtime/)
///   [KernelEntry]        → called from extern "C" neko_kernel_main(BootInfo*)
/// </summary>
sealed partial class CppEmitter
{
    readonly CSharpCompilation _compilation;
    readonly DiagnosticSink _diag;
    readonly ProjectConfig _config;
    readonly TypeMapper _types;
    readonly Dictionary<SyntaxTree, SemanticModel> _models = [];

    readonly List<INamedTypeSymbol> _enums = [];
    readonly List<INamedTypeSymbol> _structs = [];
    readonly List<INamedTypeSymbol> _staticClasses = [];
    IMethodSymbol? _entry;
    readonly List<(IMethodSymbol Method, int Line)> _irqHandlers = [];

    SemanticModel _model = null!;
    bool _inFunction;
    int _uniqueCounter;

    public CppEmitter(CSharpCompilation compilation, DiagnosticSink diag, ProjectConfig config)
    {
        _compilation = compilation;
        _diag = diag;
        _config = config;
        _types = new TypeMapper(diag);
    }

    SemanticModel ModelFor(SyntaxTree tree)
    {
        if (!_models.TryGetValue(tree, out var model))
            _models[tree] = model = _compilation.GetSemanticModel(tree);
        return model;
    }

    void UseModel(SyntaxNode node) => _model = ModelFor(node.SyntaxTree);

    string Map(ITypeSymbol? type, SyntaxNode? at) => _types.Map(type, at);
    string Qualified(INamedTypeSymbol type) => _types.Qualified(type);

    public (string Header, string Source) Emit()
    {
        Collect();
        FindEntry();
        FindIrqHandlers();

        var header = new CodeWriter();
        var source = new CodeWriter();
        EmitHeader(header);
        EmitSource(source);
        return (header.ToString(), source.ToString());
    }

    // ================================================================ collection

    void Collect()
    {
        var seen = new HashSet<INamedTypeSymbol>(SymbolEqualityComparer.Default);
        foreach (var tree in _compilation.SyntaxTrees)
        {
            var model = ModelFor(tree);
            foreach (var decl in tree.GetRoot().DescendantNodes().OfType<MemberDeclarationSyntax>())
            {
                if (decl is DelegateDeclarationSyntax del)
                {
                    _diag.Error("NEKO0100", "delegates are not supported — use function pointers (delegate* unmanaged<...>)", del.Identifier.GetLocation());
                    continue;
                }
                if (decl is not BaseTypeDeclarationSyntax typeDecl)
                    continue;
                if (model.GetDeclaredSymbol(typeDecl) is not INamedTypeSymbol type || !seen.Add(type))
                    continue;
                if (IsSkipped(type))
                    continue;

                var loc = typeDecl.Identifier.GetLocation();
                if (type.ContainingType is { } outer && !IsSkipped(outer))
                {
                    _diag.Error("NEKO0100", $"nested type '{type.Name}' is not supported yet — move it to namespace level", loc);
                    continue;
                }
                if (type.IsGenericType)
                {
                    _diag.Error("NEKO0100", $"generic type '{type.Name}' is not supported yet (generic methods are) — see ROADMAP phase 3", loc);
                    continue;
                }

                switch (type.TypeKind)
                {
                    case TypeKind.Enum:
                        _enums.Add(type);
                        break;
                    case TypeKind.Struct when type.IsRecord:
                        _diag.Error("NEKO0100", "record structs are not supported yet", loc);
                        break;
                    case TypeKind.Struct:
                        _structs.Add(type);
                        break;
                    case TypeKind.Class when type.IsStatic:
                        _staticClasses.Add(type);
                        break;
                    case TypeKind.Class:
                        _diag.Error("NEKO0100", $"'{type.Name}' is a reference class — the kernel has no GC yet; use a struct (allocate with Heap.New<T>()) or a static class", loc);
                        break;
                    default:
                        _diag.Error("NEKO0100", $"{type.TypeKind.ToString().ToLowerInvariant()} '{type.Name}' is not supported yet", loc);
                        break;
                }
            }
        }
    }

    static bool IsSkipped(INamedTypeSymbol type) =>
        Symbols.NativeName(type) is not null || Symbols.IsIntrinsicType(type) || Symbols.IsAttributeType(type) ||
        type.TypeKind == TypeKind.Class && Symbols.IsExceptionType(type);

    /// <summary>Repository-relative path with forward slashes, for messages shown on the stop screen.</summary>
    string RelativePath(string path) => Path.GetRelativePath(_config.RootDirectory, path).Replace('\\', '/');

    /// <summary>"Nekotia.OS.Commands.Crash" for the method containing <paramref name="node"/>.</summary>
    string EnclosingMember(SyntaxNode node)
    {
        var symbol = _model.GetEnclosingSymbol(node.SpanStart);
        while (symbol is IMethodSymbol { MethodKind: MethodKind.LocalFunction or MethodKind.AnonymousFunction })
            symbol = symbol.ContainingSymbol;
        return symbol is null ? "?" : $"{symbol.ContainingType?.ToDisplayString()}.{symbol.Name}";
    }

    void FindEntry()
    {
        var entries = AllTypes()
            .SelectMany(t => t.GetMembers().OfType<IMethodSymbol>())
            .Where(m => Symbols.Has(m, "KernelEntry"))
            .ToList();

        if (entries.Count == 0)
        {
            _diag.Error("NEKO0001", "no method marked [KernelEntry] — add `[KernelEntry] public static void Start(BootInfo* info)`", (Location?)null);
            return;
        }
        if (entries.Count > 1)
        {
            foreach (var e in entries)
                _diag.Error("NEKO0001", "only one [KernelEntry] method is allowed", e.Locations.FirstOrDefault());
            return;
        }

        var entry = entries[0];
        bool okParams = entry.Parameters.Length == 0 ||
                        (entry.Parameters.Length == 1 && entry.Parameters[0].Type is IPointerTypeSymbol { PointedAtType: var pt } &&
                         Symbols.NativeName(pt) == "::neko::BootInfo");
        if (!entry.IsStatic || !entry.ReturnsVoid || !okParams || entry.IsGenericMethod)
        {
            _diag.Error("NEKO0001", "[KernelEntry] must be `static void Name()` or `static void Name(BootInfo* info)`", entry.Locations.FirstOrDefault());
            return;
        }
        _entry = entry;
    }

    void FindIrqHandlers()
    {
        var used = new Dictionary<int, IMethodSymbol>();
        foreach (var m in AllTypes().SelectMany(t => t.GetMembers().OfType<IMethodSymbol>()))
        {
            if (Symbols.Attr(m, "Irq") is not { ConstructorArguments: [{ Value: int line }, ..] })
                continue;
            var loc = m.Locations.FirstOrDefault();
            if (!m.IsStatic || !m.ReturnsVoid || m.Parameters.Length != 0 || m.IsGenericMethod)
                _diag.Error("NEKO0003", "[Irq] handlers must be `static void Name()`", loc);
            else if (line is < 0 or > 15)
                _diag.Error("NEKO0003", "[Irq] line must be a legacy ISA IRQ 0–15", loc);
            else if (used.TryGetValue(line, out var other))
                _diag.Error("NEKO0003", $"IRQ {line} already has a handler: {other.ContainingType.Name}.{other.Name}", loc);
            else
            {
                used[line] = m;
                _irqHandlers.Add((m, line));
            }
        }
    }

    IEnumerable<INamedTypeSymbol> AllTypes() => _structs.Concat(_staticClasses);

    // ================================================================ header

    void EmitHeader(CodeWriter w)
    {
        w.Line("// <auto-generated> by nekoc — do not edit. Source: the C# kernel (see nekotia.json).");
        w.Line("#pragma once");
        w.Line("#include <neko/runtime.hpp>");
        w.Line();

        w.Line("// ---- forward declarations");
        var byNamespace = AllTypes().OrderBy(t => Symbols.NamespacePath(t.ContainingNamespace), StringComparer.Ordinal);
        ForEachInNamespace(w, byNamespace, (cw, t) => cw.Line($"struct {Symbols.Escape(t.Name)};"));
        w.Line();

        if (_enums.Count > 0)
        {
            w.Line("// ---- enums");
            ForEachInNamespace(w, _enums, EmitEnum);
            w.Line();
        }

        w.Line("// ---- structs (in field-dependency order)");
        ForEachInNamespace(w, SortStructs(), EmitTypeDeclaration);
        w.Line();

        w.Line("// ---- static classes");
        ForEachInNamespace(w, _staticClasses, EmitTypeDeclaration);
        w.Line();

        w.Line("extern \"C\" void neko_kernel_main(::neko::BootInfo* info);");
    }

    /// <summary>Emits <paramref name="body"/> for each type, wrapping runs of types from the same namespace in one block; empty blocks are dropped.</summary>
    static void ForEachInNamespace(CodeWriter w, IEnumerable<INamedTypeSymbol> types, Action<CodeWriter, INamedTypeSymbol> body)
    {
        string? current = null;
        CodeWriter? buffer = null;

        void Flush()
        {
            if (buffer is null || buffer.IsEmpty)
                return;
            if (current!.Length == 0)
            {
                w.Append(buffer);
                return;
            }
            w.Line($"namespace {current} {{");
            w.Append(buffer);
            w.Line("}");
        }

        foreach (var type in types)
        {
            var ns = Symbols.NamespacePath(type.ContainingNamespace);
            if (ns != current)
            {
                Flush();
                current = ns;
                buffer = new CodeWriter(ns.Length == 0 ? 0 : 1);
            }
            body(buffer!, type);
        }
        Flush();
    }

    void EmitEnum(CodeWriter w, INamedTypeSymbol e)
    {
        var name = Symbols.Escape(e.Name);
        var underlying = Map(e.EnumUnderlyingType, null);
        w.OpenBrace($"enum class {name} : {underlying}");
        foreach (var f in e.GetMembers().OfType<IFieldSymbol>().Where(f => f.HasConstantValue))
            w.Line($"{Symbols.Escape(f.Name)} = {Constant(f.ConstantValue, e.EnumUnderlyingType)},");
        w.CloseBrace(";");
        w.Line($"NEKO_ENUM_OPERATORS({name})");
    }

    List<INamedTypeSymbol> SortStructs()
    {
        var set = new HashSet<INamedTypeSymbol>(_structs, SymbolEqualityComparer.Default);
        var done = new HashSet<INamedTypeSymbol>(SymbolEqualityComparer.Default);
        var result = new List<INamedTypeSymbol>();

        void Visit(INamedTypeSymbol t)
        {
            if (!done.Add(t))
                return;
            foreach (var f in t.GetMembers().OfType<IFieldSymbol>())
                if (!f.IsStatic && f.Type is INamedTypeSymbol ft && set.Contains(ft))
                    Visit(ft);
            result.Add(t);
        }

        foreach (var s in _structs)
            Visit(s);
        return result;
    }

    void EmitTypeDeclaration(CodeWriter w, INamedTypeSymbol type)
    {
        var name = Symbols.Escape(type.Name);
        bool isStruct = type.TypeKind == TypeKind.Struct;

        var head = "struct ";
        if (Symbols.Has(type, "Packed"))
            head += "__attribute__((packed)) ";
        if (Symbols.Attr(type, "Align") is { ConstructorArguments: [{ Value: int align }, ..] })
            head += $"alignas({align}) ";
        w.OpenBrace(head + name);

        if (!isStruct)
            w.Line($"{name}() = delete;");

        var operators = new List<IMethodSymbol>();
        foreach (var member in type.GetMembers())
        {
            if (member.IsImplicitlyDeclared)
                continue;
            switch (member)
            {
                case IFieldSymbol f:
                    EmitFieldDeclaration(w, f);
                    break;
                case IPropertySymbol p:
                    EmitPropertyDeclaration(w, p);
                    break;
                case IMethodSymbol m:
                    switch (m.MethodKind)
                    {
                        case MethodKind.Ordinary:
                            if (Symbols.NativeName(m) is null)
                                w.Line(MethodDeclaration(m) + ";");
                            break;
                        case MethodKind.Constructor:
                            break; // below
                        case MethodKind.UserDefinedOperator:
                            operators.Add(m);
                            break;
                        case MethodKind.PropertyGet:
                        case MethodKind.PropertySet:
                            break; // emitted with the property
                        case MethodKind.StaticConstructor:
                            _diag.Error("NEKO0101", "static constructors are not supported — use field initializers or an explicit Init() method", m.Locations.FirstOrDefault());
                            break;
                        default:
                            _diag.Error("NEKO0101", $"{m.MethodKind} members are not supported yet", m.Locations.FirstOrDefault());
                            break;
                    }
                    break;
                case INamedTypeSymbol:
                    break; // nested types are reported in Collect()
                default:
                    _diag.Error("NEKO0101", $"{member.Kind.ToString().ToLowerInvariant()} members are not supported", member.Locations.FirstOrDefault());
                    break;
            }
        }

        if (isStruct)
        {
            var ctors = type.InstanceConstructors.Where(c => !c.IsImplicitlyDeclared).ToList();
            if (ctors.Count > 0 && ctors.All(c => c.Parameters.Length > 0))
                w.Line($"{name}() = default;");
            foreach (var c in ctors)
                w.Line($"{name}({Parameters(c, withDefaults: true)});");
        }

        w.CloseBrace(";");

        foreach (var op in operators)
            w.Line(OperatorSignature(op, withDefaults: true) + ";");
    }

    void EmitFieldDeclaration(CodeWriter w, IFieldSymbol f)
    {
        if (Symbols.NativeName(f) is not null && f.IsStatic)
            return;

        var name = FieldName(f);
        var at = f.DeclaringSyntaxReferences.FirstOrDefault()?.GetSyntax();

        if (f.IsConst)
        {
            w.Line($"static constexpr {Map(f.Type, at)} {name} = {Constant(f.ConstantValue, f.Type)};");
            return;
        }
        if (f.IsFixedSizeBuffer && f.Type is IPointerTypeSymbol elem)
        {
            w.Line($"{Map(elem.PointedAtType, at)} {name}[{f.FixedSize}]{{}};");
            return;
        }
        var vol = f.IsVolatile ? "volatile " : "";
        if (f.IsStatic)
        {
            w.Line($"static {vol}{Map(f.Type, at)} {name};");
            return;
        }

        // Instance field: C# zero-initialises struct fields, so do the same.
        var init = "{}";
        if (at is VariableDeclaratorSyntax { Initializer: { } initializer })
        {
            UseModel(initializer);
            var cv = _model.GetConstantValue(initializer.Value);
            if (cv.HasValue)
                init = " = " + Constant(cv.Value, f.Type);
            else
                _diag.Error("NEKO0101", "instance field initializers must be constants", initializer.GetLocation());
        }
        w.Line($"{vol}{Map(f.Type, at)} {name}{init};");
    }

    void EmitPropertyDeclaration(CodeWriter w, IPropertySymbol p)
    {
        if (p.IsIndexer)
        {
            _diag.Error("NEKO0101", "indexers are not supported yet", p.Locations.FirstOrDefault());
            return;
        }
        var stat = p.IsStatic ? "static " : "";
        var type = Map(p.Type, null);
        if (IsAutoProperty(p))
            w.Line($"{stat}{type} {BackingField(p)}{(p.IsStatic ? "" : "{}")};");
        if (p.GetMethod is not null)
            w.Line($"{stat}{type} get_{Symbols.Escape(p.Name)}();");
        if (p.SetMethod is not null)
            w.Line($"{stat}void set_{Symbols.Escape(p.Name)}({type} value);");
    }

    static bool IsAutoProperty(IPropertySymbol p) =>
        p.DeclaringSyntaxReferences.FirstOrDefault()?.GetSyntax() is PropertyDeclarationSyntax
        {
            ExpressionBody: null,
            AccessorList: { } list,
        } && list.Accessors.All(a => a.Body is null && a.ExpressionBody is null);

    static string BackingField(IPropertySymbol p) => "_k_" + p.Name;

    string FieldName(IFieldSymbol f) => Symbols.NativeName(f) ?? Symbols.Escape(f.Name);

    // ================================================================ signatures

    string MethodDeclaration(IMethodSymbol m)
    {
        var prefix = TemplatePrefix(m) + MethodAttributes(m) + (m.IsStatic ? "static " : "");
        return $"{prefix}{Map(m.ReturnType, null)} {Symbols.Escape(m.Name)}({Parameters(m, withDefaults: true)})";
    }

    static string MethodAttributes(IMethodSymbol m)
    {
        var s = "";
        if (Symbols.Has(m, "NoReturn"))
            s += "[[noreturn]] ";
        if (Symbols.Has(m, "Inline"))
            s += "[[gnu::always_inline]] ";
        return s;
    }

    static string TemplatePrefix(IMethodSymbol m) => m.IsGenericMethod
        ? "template<" + string.Join(", ", m.TypeParameters.Select(t => "typename " + Symbols.Escape(t.Name))) + "> "
        : "";

    string Parameters(IMethodSymbol m, bool withDefaults) =>
        string.Join(", ", m.Parameters.Select(p => Parameter(p, withDefaults)));

    string Parameter(IParameterSymbol p, bool withDefaults)
    {
        if (p.IsParams)
            _diag.Error("NEKO0101", "params arrays are not supported", p.Locations.FirstOrDefault());

        var type = Map(p.Type, null);
        type = p.RefKind switch
        {
            RefKind.Ref or RefKind.Out => type + "&",
            RefKind.In or RefKind.RefReadOnlyParameter => "const " + type + "&",
            _ => type,
        };
        var s = $"{type} {Symbols.Escape(p.Name)}";
        if (withDefaults && p.HasExplicitDefaultValue)
            s += " = " + Constant(p.ExplicitDefaultValue, p.Type);
        return s;
    }

    string OperatorSignature(IMethodSymbol op, bool withDefaults)
    {
        var syntax = op.DeclaringSyntaxReferences[0].GetSyntax() as OperatorDeclarationSyntax;
        var token = syntax?.OperatorToken.Text ?? "?";
        return $"{Map(op.ReturnType, null)} operator{token}({Parameters(op, withDefaults)})";
    }

    // ================================================================ source

    void EmitSource(CodeWriter w)
    {
        w.Line("// <auto-generated> by nekoc — do not edit. Source: the C# kernel (see nekotia.json).");
        w.Line($"#include \"{Driver.HeaderName}\"");
        w.Line();

        w.Line("// ---- static fields");
        ForEachInNamespace(w, AllTypes(), EmitStaticFieldDefinitions);
        w.Line();

        w.Line("// ---- methods");
        ForEachInNamespace(w, AllTypes(), EmitMemberDefinitions);
        w.Line();

        w.Line("// ---- [Irq] handlers (registered before interrupts are enabled)");
        w.OpenBrace("extern \"C\" void neko_kernel_init()");
        foreach (var (method, line) in _irqHandlers)
        {
            var target = $"{Qualified(method.ContainingType)}::{Symbols.Escape(method.Name)}";
            w.Line($"if (!::neko::irq::register_handler({line}, &{target})) ::neko::term::write(\"[neko] warning: IRQ {line} handler {method.ContainingType.Name}.{method.Name} not installed\\n\");");
        }
        w.CloseBrace();
        w.Line();

        w.Line("// ---- entry point (called by runtime/src/arch/*/boot.cpp)");
        w.OpenBrace("extern \"C\" void neko_kernel_main(::neko::BootInfo* info)");
        if (_entry is not null)
        {
            var target = $"{Qualified(_entry.ContainingType)}::{Symbols.Escape(_entry.Name)}";
            w.Line(_entry.Parameters.Length == 1 ? $"{target}(info);" : $"(void)info; {target}();");
        }
        w.CloseBrace();
    }

    void EmitStaticFieldDefinitions(CodeWriter w, INamedTypeSymbol type)
    {
        var typeName = Symbols.Escape(type.Name);
        foreach (var member in type.GetMembers())
        {
            if (member is IFieldSymbol { IsStatic: true, IsConst: false, IsImplicitlyDeclared: false } f && Symbols.NativeName(f) is null)
            {
                var decl = f.DeclaringSyntaxReferences.FirstOrDefault()?.GetSyntax() as VariableDeclaratorSyntax;
                var vol = f.IsVolatile ? "volatile " : "";
                w.Line($"{vol}{Map(f.Type, decl)} {typeName}::{FieldName(f)}{StaticInitializer(decl?.Initializer?.Value)};");
            }
            else if (member is IPropertySymbol { IsStatic: true } p && IsAutoProperty(p))
            {
                var decl = p.DeclaringSyntaxReferences[0].GetSyntax() as PropertyDeclarationSyntax;
                w.Line($"{Map(p.Type, decl)} {typeName}::{BackingField(p)}{StaticInitializer(decl?.Initializer?.Value)};");
            }
        }
    }

    string StaticInitializer(ExpressionSyntax? value)
    {
        if (value is null)
            return "{}";
        UseModel(value);
        _inFunction = false;
        return " = " + ExprAs(value);
    }

    void EmitMemberDefinitions(CodeWriter w, INamedTypeSymbol type)
    {
        var typeName = Symbols.Escape(type.Name);
        foreach (var member in type.GetMembers())
        {
            if (member.IsImplicitlyDeclared)
                continue;

            switch (member)
            {
                case IMethodSymbol { MethodKind: MethodKind.Ordinary } m when Symbols.NativeName(m) is null:
                {
                    var impl = m.PartialImplementationPart ?? m;
                    if (impl.DeclaringSyntaxReferences.FirstOrDefault()?.GetSyntax() is not BaseMethodDeclarationSyntax syntax)
                        continue;
                    if (syntax.Body is null && syntax.ExpressionBody is null)
                    {
                        _diag.Error("NEKO0101", $"method '{m.Name}' has no body — mark it [Native(\"cpp::name\")] if it is implemented in the runtime", m.Locations.FirstOrDefault());
                        continue;
                    }
                    var head = $"{TemplatePrefix(m)}{Map(m.ReturnType, syntax)} {typeName}::{Symbols.Escape(m.Name)}({Parameters(m, withDefaults: false)})";
                    EmitFunction(w, head, syntax, syntax.Body, syntax.ExpressionBody, m.ReturnsVoid);
                    break;
                }

                case IMethodSymbol { MethodKind: MethodKind.Constructor } c:
                {
                    if (c.DeclaringSyntaxReferences.FirstOrDefault()?.GetSyntax() is not ConstructorDeclarationSyntax syntax)
                        continue;
                    var head = $"{typeName}::{typeName}({Parameters(c, withDefaults: false)})";
                    if (syntax.Initializer is { } ci)
                    {
                        UseModel(ci);
                        if (ci.IsKind(SyntaxKind.BaseConstructorInitializer))
                            _diag.Error("NEKO0101", "base(...) constructor initializers are not supported", ci);
                        else if (_model.GetOperation(ci) is Microsoft.CodeAnalysis.Operations.IInvocationOperation call)
                            head += $" : {typeName}({Arguments(call.Arguments, call.TargetMethod)})";
                    }
                    EmitFunction(w, head, syntax, syntax.Body, syntax.ExpressionBody, returnsVoid: true);
                    break;
                }

                case IMethodSymbol { MethodKind: MethodKind.UserDefinedOperator } op:
                {
                    var syntax = (OperatorDeclarationSyntax)op.DeclaringSyntaxReferences[0].GetSyntax();
                    EmitFunction(w, OperatorSignature(op, withDefaults: false), syntax, syntax.Body, syntax.ExpressionBody, op.ReturnsVoid);
                    break;
                }

                case IPropertySymbol p when !p.IsIndexer:
                    EmitPropertyDefinitions(w, typeName, p);
                    break;
            }
        }
    }

    void EmitPropertyDefinitions(CodeWriter w, string typeName, IPropertySymbol p)
    {
        var type = Map(p.Type, null);
        var name = Symbols.Escape(p.Name);
        var syntax = (PropertyDeclarationSyntax)p.DeclaringSyntaxReferences[0].GetSyntax();

        if (IsAutoProperty(p))
        {
            if (p.GetMethod is not null)
                w.Line($"{type} {typeName}::get_{name}() {{ return {BackingField(p)}; }}");
            if (p.SetMethod is not null)
                w.Line($"void {typeName}::set_{name}({type} value) {{ {BackingField(p)} = value; }}");
            return;
        }

        if (syntax.ExpressionBody is { } arrow)
        {
            EmitFunction(w, $"{type} {typeName}::get_{name}()", syntax, null, arrow, returnsVoid: false);
            return;
        }

        foreach (var accessor in syntax.AccessorList!.Accessors)
        {
            if (accessor.IsKind(SyntaxKind.GetAccessorDeclaration))
                EmitFunction(w, $"{type} {typeName}::get_{name}()", accessor, accessor.Body, accessor.ExpressionBody, returnsVoid: false);
            else if (accessor.IsKind(SyntaxKind.SetAccessorDeclaration) || accessor.IsKind(SyntaxKind.InitAccessorDeclaration))
                EmitFunction(w, $"void {typeName}::set_{name}({type} value)", accessor, accessor.Body, accessor.ExpressionBody, returnsVoid: true);
        }
    }

    void EmitFunction(CodeWriter w, string head, SyntaxNode decl, BlockSyntax? body, ArrowExpressionClauseSyntax? arrow, bool returnsVoid)
    {
        UseModel(decl);
        _inFunction = true;
        w.Line();
        LineDirective(w, decl);
        w.OpenBrace(head);
        if (body is not null)
        {
            foreach (var s in body.Statements)
                Statement(w, s);
        }
        else if (arrow is not null)
        {
            LineDirective(w, arrow.Expression);
            w.Line(returnsVoid ? $"{Expr(arrow.Expression)};" : $"return {ExprAs(arrow.Expression)};");
        }
        w.CloseBrace();
        _inFunction = false;
    }

    void LineDirective(CodeWriter w, SyntaxNode node)
    {
        if (!_config.LineDirectives)
            return;
        var span = node.GetLocation().GetLineSpan();
        var path = span.Path.Replace("\\", "\\\\").Replace("\"", "\\\"");
        w.Directive($"#line {span.StartLinePosition.Line + 1} \"{path}\"");
    }
}
