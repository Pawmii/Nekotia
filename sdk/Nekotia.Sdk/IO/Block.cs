namespace Nekotia.IO;

/// <summary>A disk. Mirrors <c>neko::block::Info</c>.</summary>
[Native("::neko::block::Info")]
public unsafe struct BlockInfo
{
    [Native("name")] public fixed byte Name[16];
    [Native("model")] public fixed byte Model[40];
    [Native("sectors")] public ulong Sectors;
    [Native("sector_size")] public uint SectorSize;
    [Native("read_only")] public bool ReadOnly;
    [Native("read_sectors")] public ulong ReadSectors;
    [Native("written_sectors")] public ulong WrittenSectors;
    /// <summary>Unplugged: every access fails.</summary>
    [Native("removed")] public bool Removed;
}

/// <summary>Mirrors <c>neko::block::CacheStats</c> (counts are 4 KiB lines).</summary>
[Native("::neko::block::CacheStats")]
public struct BlockCacheStats
{
    [Native("lines")] public ulong Lines;
    [Native("dirty")] public ulong Dirty;
    [Native("capacity")] public ulong Capacity;
    [Native("hits")] public ulong Hits;
    [Native("misses")] public ulong Misses;
    [Native("written_back")] public ulong WrittenBack;
}

/// <summary>
/// Block devices (runtime/src/core/block.cpp): virtio-blk disks (vda, vdb…) and RAM disks, addressed by index.
/// Each is also /dev/NAME in the VFS. Results are 0 / byte counts, or negative errors.
/// </summary>
public static unsafe class Block
{
    [Native("::neko::block::count")]
    public static extern uint Count();

    /// <summary>Writes every disk's cached changes back (then flushes the devices); 0 or an error.</summary>
    [Native("::neko::block::sync_all")]
    public static extern long SyncAll();

    [Native("::neko::block::cache_stats")]
    public static extern BlockCacheStats CacheStats();

    /// <summary>Called (from a driver thread) when a device is added or removed — USB sticks.</summary>
    [Native("::neko::block::set_hotplug")]
    public static extern void SetHotplug(delegate*<uint, bool, void> handler);

    [Native("::neko::block::info")]
    public static extern bool GetInfo(uint index, out BlockInfo info);

    /// <summary>Index of the device called <paramref name="name"/>, -1 if absent.</summary>
    [Native("::neko::block::find")]
    public static extern int Find(string name);

    [Native("::neko::block::find")]
    public static extern int Find(byte* name);

    /// <summary>Reads <paramref name="count"/> sectors at <paramref name="lba"/>; 0 or an error.</summary>
    [Native("::neko::block::read")]
    public static extern long Read(uint index, ulong lba, uint count, void* buffer);

    [Native("::neko::block::write")]
    public static extern long Write(uint index, ulong lba, uint count, void* buffer);

    [Native("::neko::block::flush")]
    public static extern long Flush(uint index);

    /// <summary>Byte-addressed access; bytes transferred (short at the end of the disk).</summary>
    [Native("::neko::block::read_bytes")]
    public static extern long ReadBytes(uint index, ulong offset, void* buffer, ulong length);

    [Native("::neko::block::write_bytes")]
    public static extern long WriteBytes(uint index, ulong offset, void* buffer, ulong length);

    /// <summary>A zeroed disk in the kernel heap (never freed); its index or an error.</summary>
    [Native("::neko::block::add_ramdisk")]
    public static extern int AddRamDisk(string name, ulong bytes);
}
