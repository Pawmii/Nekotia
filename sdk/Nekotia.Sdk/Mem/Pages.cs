namespace Nekotia.Mem;

/// <summary>
/// Physical memory: a buddy allocator over 4 KiB frames (runtime/src/core/pmm.cpp).
/// Pointers are HHDM virtual addresses. Blocks are physically contiguous and aligned to their size.
/// </summary>
public static unsafe class Pages
{
    public const ulong Size = 4096;
    public const int MaxOrder = 10;   // 2^10 pages = 4 MiB

    /// <summary>One zeroed 4 KiB frame, or null when memory is exhausted.</summary>
    [Native("::neko::pmm::alloc_page")]
    public static extern void* Alloc();

    [Native("::neko::pmm::free_page")]
    public static extern void Free(void* page);

    /// <summary><paramref name="count"/> physically contiguous zeroed frames (rounded up to a power of two) — for DMA buffers.</summary>
    [Native("::neko::pmm::alloc_contiguous")]
    public static extern void* AllocContiguous(ulong count);

    [Native("::neko::pmm::free_contiguous")]
    public static extern void FreeContiguous(void* block, ulong count);

    /// <summary>Free blocks of 2^<paramref name="order"/> pages.</summary>
    [Native("::neko::pmm::free_blocks")]
    public static extern ulong FreeBlocks(uint order);

    /// <summary>Order of the largest free block (uint.MaxValue when memory is exhausted).</summary>
    [Native("::neko::pmm::largest_free_order")]
    public static extern uint LargestFreeOrder();

    [Native("::neko::pmm::free_pages")]
    public static extern ulong FreeCount();

    [Native("::neko::pmm::total_pages")]
    public static extern ulong TotalCount();

    [Native("::neko::pmm::virt_to_phys")]
    public static extern ulong VirtToPhys(void* address);

    [Native("::neko::pmm::phys_to_virt")]
    public static extern void* PhysToVirt(ulong physical);
}

/// <summary>Raw memory helpers.</summary>
public static unsafe class Memory
{
    [Native("::neko::mem::set")]
    public static extern void Set(void* destination, byte value, nuint count);

    [Native("::neko::mem::copy")]
    public static extern void Copy(void* destination, void* source, nuint count);

    [Native("::neko::mem::move")]
    public static extern void Move(void* destination, void* source, nuint count);

    [Native("::neko::mem::compare")]
    public static extern int Compare(void* a, void* b, nuint count);

    public static void Zero(void* destination, nuint count) => Set(destination, 0, count);
}
