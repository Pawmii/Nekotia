using Microsoft.CodeAnalysis;

namespace Nekotia.Compiler;

/// <summary>Maps C# types to their C++ spelling.</summary>
sealed class TypeMapper(DiagnosticSink diag)
{
    public string Map(ITypeSymbol? type, SyntaxNode? at)
    {
        switch (type)
        {
            case null:
                diag.Error("NEKO0201", "could not resolve type", at?.GetLocation());
                return "void";

            case IPointerTypeSymbol p:
                return Map(p.PointedAtType, at) + "*";

            case IFunctionPointerTypeSymbol f:
            {
                var sig = f.Signature;
                var parts = new List<string> { Map(sig.ReturnType, at) };
                parts.AddRange(sig.Parameters.Select(p => Map(p.Type, at)));
                return $"::neko::fnptr<{string.Join(", ", parts)}>";
            }

            case ITypeParameterSymbol tp:
                return Symbols.Escape(tp.Name);

            case IArrayTypeSymbol:
                diag.Error("NEKO0201", $"managed arrays ('{type.ToDisplayString()}') are not supported yet — use a pointer from Heap.Alloc, stackalloc, or a fixed buffer", at?.GetLocation());
                return "void*";
        }

        switch (type.SpecialType)
        {
            case SpecialType.System_Void: return "void";
            case SpecialType.System_Boolean: return "bool";
            case SpecialType.System_Char: return "char16_t";
            case SpecialType.System_SByte: return "int8_t";
            case SpecialType.System_Byte: return "uint8_t";
            case SpecialType.System_Int16: return "int16_t";
            case SpecialType.System_UInt16: return "uint16_t";
            case SpecialType.System_Int32: return "int32_t";
            case SpecialType.System_UInt32: return "uint32_t";
            case SpecialType.System_Int64: return "int64_t";
            case SpecialType.System_UInt64: return "uint64_t";
            case SpecialType.System_IntPtr: return "intptr_t";
            case SpecialType.System_UIntPtr: return "uintptr_t";
            case SpecialType.System_String: return "const char*";
            case SpecialType.System_Single: return "float";
            case SpecialType.System_Double: return "double";
            case SpecialType.System_Decimal:
                diag.Error("NEKO0201", "decimal is not supported — use double or fixed-point integers", at?.GetLocation());
                return "double";
        }

        if (type is INamedTypeSymbol named)
        {
            if (Symbols.NativeName(named) is { } native)
                return native;
            if (Symbols.IsInSource(named) && named.TypeKind is TypeKind.Struct or TypeKind.Enum)
                return Qualified(named);
            if (Symbols.IsInSource(named) && named.TypeKind == TypeKind.Class && named.IsStatic)
                return Qualified(named);
        }

        diag.Error("NEKO0201", $"type '{type.ToDisplayString()}' is not supported by nekoc (kernel and user programs)", at?.GetLocation());
        return "void";
    }

    public string Qualified(INamedTypeSymbol type)
    {
        if (Symbols.NativeName(type) is { } native)
            return native;
        var ns = Symbols.NamespacePath(type.ContainingNamespace);
        return ns.Length == 0 ? "::" + Symbols.Escape(type.Name) : $"::{ns}::{Symbols.Escape(type.Name)}";
    }

    public static bool IsFloating(ITypeSymbol? t) => t?.SpecialType is SpecialType.System_Single or SpecialType.System_Double;

    public static bool IsNumeric(ITypeSymbol? t) => t?.SpecialType is
        SpecialType.System_SByte or SpecialType.System_Byte or SpecialType.System_Int16 or SpecialType.System_UInt16 or
        SpecialType.System_Int32 or SpecialType.System_UInt32 or SpecialType.System_Int64 or SpecialType.System_UInt64 or
        SpecialType.System_IntPtr or SpecialType.System_UIntPtr or SpecialType.System_Char;
}
