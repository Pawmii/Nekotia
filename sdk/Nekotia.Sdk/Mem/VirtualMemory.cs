using System;

namespace Nekotia.Mem;

/// <summary>Page permissions. Mirrors <c>neko::vmm::PageFlags</c> (runtime/include/neko/memory.hpp).</summary>
[Flags]
[Native("::neko::vmm::PageFlags")]
public enum PageFlags : uint
{
    /// <summary>Read-only, not executable.</summary>
    None = 0,
    Write = 1,
    Execute = 2,
    User = 4,
    /// <summary>For MMIO registers.</summary>
    Uncached = 8,
    /// <summary>For framebuffers.</summary>
    WriteCombining = 16,
}

/// <summary>
/// The kernel's page tables (runtime/src/arch/x86_64/vmm.cpp).
/// Layout: HHDM 0xffff800000000000 · heap 0xffffc00000000000 · free 0xffffd00000000000 ·
/// stacks 0xffffe00000000000 · kernel image 0xffffffff80000000.
/// Not available in host mode (Ready() is false there).
/// </summary>
public static class VirtualMemory
{
    public const ulong HeapBase = 0xffffc00000000000;
    /// <summary>Start of the range free for drivers and experiments.</summary>
    public const ulong FreeBase = 0xffffd00000000000;
    public const ulong StackBase = 0xffffe00000000000;
    public const ulong Unmapped = ulong.MaxValue;

    [Native("::neko::vmm::ready")]
    public static extern bool Ready();

    /// <summary>Maps one 4 KiB page; false when <paramref name="virt"/> is already mapped.</summary>
    [Native("::neko::vmm::map")]
    public static extern bool Map(ulong virt, ulong phys, PageFlags flags);

    /// <summary>Removes the mapping (the physical frame is not freed).</summary>
    [Native("::neko::vmm::unmap")]
    public static extern bool Unmap(ulong virt);

    [Native("::neko::vmm::protect")]
    public static extern bool Protect(ulong virt, PageFlags flags);

    /// <summary>Physical address behind <paramref name="virt"/>, or <see cref="Unmapped"/>.</summary>
    [Native("::neko::vmm::translate")]
    public static extern ulong Translate(ulong virt);

    /// <summary>Allocates frames and maps them at [virt, virt + size).</summary>
    [Native("::neko::vmm::map_fresh")]
    public static extern bool MapFresh(ulong virt, nuint size, PageFlags flags);

    [Native("::neko::vmm::is_stack_guard")]
    public static extern bool IsStackGuard(ulong address);
}
