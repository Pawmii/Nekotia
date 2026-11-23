namespace Nekotia.Mem;

/// <summary>
/// Kernel heap — the C++ free-list allocator in runtime/src/core/heap.cpp.
/// Blocks are 16-byte aligned, freed blocks coalesce with their neighbours, and corruption or a double free panics.
/// </summary>
public static unsafe class Heap
{
    [Native("::neko::heap::alloc")]
    public static extern void* Alloc(nuint size);

    /// <summary>Like Alloc, but the memory is zero-filled (what C# code usually expects).</summary>
    [Native("::neko::heap::alloc_zeroed")]
    public static extern void* AllocZeroed(nuint size);

    [Native("::neko::heap::realloc")]
    public static extern void* Realloc(void* block, nuint newSize);

    [Native("::neko::heap::free")]
    public static extern void Free(void* block);

    [Native("::neko::heap::used_bytes")]
    public static extern nuint UsedBytes();

    [Native("::neko::heap::free_bytes")]
    public static extern nuint FreeBytes();

    [Native("::neko::heap::total_bytes")]
    public static extern nuint TotalBytes();

    [Native("::neko::heap::block_count")]
    public static extern nuint BlockCount();

    /// <summary>Walks every block and verifies headers; returns false on corruption.</summary>
    [Native("::neko::heap::check")]
    public static extern bool Check();

    // C# helpers (generic → C++ template).

    /// <summary>Allocates one zeroed <typeparamref name="T"/>. Free it with <see cref="Delete{T}"/>.</summary>
    public static T* New<T>() where T : unmanaged => (T*)AllocZeroed((nuint)sizeof(T));

    /// <summary>Allocates <paramref name="count"/> zeroed elements.</summary>
    public static T* NewArray<T>(nuint count) where T : unmanaged => (T*)AllocZeroed(count * (nuint)sizeof(T));

    public static void Delete<T>(T* value) where T : unmanaged => Free(value);
}
