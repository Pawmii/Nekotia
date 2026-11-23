namespace Nekotia.Mem;

/// <summary>Physical page frame allocator (4 KiB pages, runtime/src/core/pmm.cpp). Pointers are HHDM virtual addresses.</summary>
public static unsafe class Pages
{
    public const ulong Size = 4096;

    [Native("::neko::pmm::alloc_page")]
    public static extern void* Alloc();

    [Native("::neko::pmm::free_page")]
    public static extern void Free(void* page);

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
