using Microsoft.CodeAnalysis;

namespace Nekotia.Compiler;

/// <summary>Helpers for the Nekotia SDK attributes and C++ naming.</summary>
static class Symbols
{
    public static AttributeData? Attr(ISymbol symbol, string name) =>
        symbol.GetAttributes().FirstOrDefault(a => a.AttributeClass?.ToDisplayString() == $"Nekotia.{name}Attribute");

    public static bool Has(ISymbol symbol, string name) => Attr(symbol, name) is not null;

    /// <summary>The C++ name from <c>[Native("...")]</c>, or null.</summary>
    public static string? NativeName(ISymbol symbol) =>
        Attr(symbol, "Native") is { ConstructorArguments: [{ Value: string name }, ..] } ? name : null;

    public static bool IsInSource(ISymbol symbol) => symbol.Locations.Any(l => l.IsInSource);

    public static bool IsIntrinsicType(INamedTypeSymbol? type) => type is not null && Has(type, "Intrinsic");

    public static bool IsAttributeType(INamedTypeSymbol type)
    {
        for (var t = type.BaseType; t is not null; t = t.BaseType)
            if (t.ToDisplayString() == "System.Attribute")
                return true;
        return false;
    }

    static readonly HashSet<string> CppReserved =
    [
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
        "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
        // common macros / libc names that would clash in the generated translation unit
        "NULL", "errno", "assert", "offsetof", "main", "unix", "linux",
    ];

    public static string Escape(string name) => CppReserved.Contains(name) ? name + "_" : name;

    public static string NamespacePath(INamespaceSymbol? ns)
    {
        var parts = new List<string>();
        for (var n = ns; n is { IsGlobalNamespace: false }; n = n.ContainingNamespace)
            parts.Add(Escape(n.Name));
        parts.Reverse();
        return string.Join("::", parts);
    }
}
