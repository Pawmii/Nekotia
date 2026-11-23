namespace Nekotia;

/// <summary>
/// Inline assembly. The argument must be a constant string.
/// In .neko files prefer the sugar: <c>asm { cli; hlt }</c> or <c>asm "pause";</c>.
/// In host builds (make run-host) assembly is compiled out.
/// </summary>
[Intrinsic]
public static class Asm
{
    /// <summary>Intel syntax, no register prefixes: <c>Asm.Emit("mov rax, cr3")</c>.</summary>
    public static extern void Emit(string code);

    /// <summary>AT&amp;T syntax: <c>Asm.EmitAtt("movq %cr3, %rax")</c>.</summary>
    public static extern void EmitAtt(string code);
}

[Intrinsic]
public static class Intrinsics
{
    /// <summary>Tells the optimizer this point is never reached (<c>__builtin_unreachable</c>).</summary>
    public static extern void Unreachable();

    /// <summary>Software breakpoint (<c>int3</c>), handy with <c>make debug</c>.</summary>
    public static extern void Breakpoint();

    /// <summary>Prevents the compiler from reordering memory accesses across this point.</summary>
    public static extern void CompilerBarrier();
}

/// <summary>Backing symbols for .neko sugar. Do not use directly.</summary>
[Intrinsic]
public static class Sugar
{
    /// <summary><c>defer stmt;</c> is lowered to <c>if (Sugar.Defer) stmt;</c> and nekoc turns it into a scope guard.</summary>
    public static readonly bool Defer = true;
}
