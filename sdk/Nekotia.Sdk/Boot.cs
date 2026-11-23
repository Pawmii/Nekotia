namespace Nekotia;

/// <summary>Linear framebuffer handed over by the bootloader. Mirrors <c>neko::Framebuffer</c> (runtime/include/neko/boot.hpp).</summary>
[Native("::neko::Framebuffer")]
public unsafe struct Framebuffer
{
    /// <summary>First pixel; null when the bootloader gave no framebuffer.</summary>
    public uint* Address;
    public ulong Width;
    public ulong Height;
    /// <summary>Bytes per scanline (may be larger than Width * 4).</summary>
    public ulong Pitch;
    public ushort Bpp;
}

/// <summary>Everything the runtime learned at boot. Mirrors <c>neko::BootInfo</c>.</summary>
[Native("::neko::BootInfo")]
public unsafe struct BootInfo
{
    /// <summary>Virtual offset of the higher-half direct map: virt = phys + HhdmOffset.</summary>
    public ulong HhdmOffset;
    /// <summary>Sum of all memory map entries, in bytes.</summary>
    public ulong TotalMemory;
    /// <summary>Usable RAM in bytes (before the heap and page allocator took their share).</summary>
    public ulong UsableMemory;
    public ulong MemoryMapEntries;
    public Framebuffer Screen;
    /// <summary>NUL-terminated strings, never null.</summary>
    public byte* BootloaderName;
    public byte* BootloaderVersion;
    /// <summary>Kernel command line from limine.conf (`cmdline:`), never null.</summary>
    public byte* CommandLine;
    /// <summary>True when running as a Linux process (make run-host).</summary>
    public bool Hosted;
}
