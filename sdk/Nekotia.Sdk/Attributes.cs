using System;

namespace Nekotia;

/// <summary>
/// Marks the kernel entry point. Exactly one method in the kernel must have it:
/// <c>[KernelEntry] public static void Start(BootInfo* info)</c> (or with no parameters).
/// Sugar: <c>@entry</c>.
/// </summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class KernelEntryAttribute : Attribute { }

/// <summary>
/// The member is implemented in the C++ runtime (runtime/). nekoc does not emit it and spells every
/// reference as <paramref name="cppName"/>. Works on methods, structs and fields.
/// Sugar: <c>@native("neko::serial::write")</c>.
/// </summary>
[AttributeUsage(AttributeTargets.Method | AttributeTargets.Struct | AttributeTargets.Field | AttributeTargets.Class | AttributeTargets.Enum)]
public sealed class NativeAttribute(string cppName) : Attribute
{
    public string CppName { get; } = cppName;
}

/// <summary>Struct without padding (<c>__attribute__((packed))</c>) — for GDT/IDT entries, ACPI tables, etc. Sugar: <c>@packed</c>.</summary>
[AttributeUsage(AttributeTargets.Struct)]
public sealed class PackedAttribute : Attribute { }

/// <summary>Struct alignment in bytes (<c>alignas(n)</c>). Sugar: <c>@align(16)</c>.</summary>
[AttributeUsage(AttributeTargets.Struct)]
public sealed class AlignAttribute(int bytes) : Attribute
{
    public int Bytes { get; } = bytes;
}

/// <summary>Always inline this method (<c>[[gnu::always_inline]]</c>). Sugar: <c>@inline</c>.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class InlineAttribute : Attribute { }

/// <summary>The method never returns (<c>[[noreturn]]</c>). Sugar: <c>@noreturn</c>.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class NoReturnAttribute : Attribute { }

/// <summary>
/// Interrupt handler for legacy ISA IRQ <paramref name="line"/> (1 = PS/2 keyboard, 4 = COM1, 12 = PS/2 mouse…).
/// nekoc registers it before interrupts are enabled. The method must be <c>static void Name()</c>, runs with
/// interrupts disabled, must be short and must not use the heap. Sugar: <c>@irq(1)</c>.
/// </summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class IrqAttribute(int line) : Attribute
{
    public int Line { get; } = line;
}

/// <summary>
/// System call <paramref name="number"/> for user programs, implemented in the kernel's C#. The method must be
/// <c>static</c>, take at most 6 integer, bool, enum or pointer parameters (the registers rdi, rsi, rdx, r10, r8, r9)
/// and return an integer (negative = error) or void (0). nekoc registers it before interrupts are enabled.
/// Numbers 0–31 belong to the runtime (neko/process.hpp) except 2 (read), which the kernel provides in C#;
/// 40–63 are free. Pointer arguments come from the program: check them with <c>UserMemory.CanRead/CanWrite</c>.
/// Sugar: <c>@syscall(32)</c>.
/// </summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class SyscallAttribute(int number) : Attribute
{
    public int Number { get; } = number;
}

/// <summary>Compiler-internal: the type is handled by nekoc itself and never emitted.</summary>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
public sealed class IntrinsicAttribute : Attribute { }
