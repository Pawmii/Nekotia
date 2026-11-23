namespace Nekotia.Hal;

/// <summary>One PCI function found at boot. Mirrors <c>neko::pci::Device</c>.</summary>
[Native("::neko::pci::Device")]
public unsafe struct PciDevice
{
    [Native("segment")] public ushort Segment;
    [Native("bus")] public byte Bus;
    [Native("device")] public byte Slot;
    [Native("function")] public byte Function;
    /// <summary>0 device, 1 PCI-to-PCI bridge, 2 CardBus bridge.</summary>
    [Native("header_type")] public byte HeaderType;
    [Native("vendor_id")] public ushort VendorId;
    [Native("device_id")] public ushort DeviceId;
    [Native("class_code")] public byte Class;
    [Native("subclass")] public byte Subclass;
    [Native("prog_if")] public byte ProgIf;
    [Native("revision")] public byte Revision;
    [Native("subsystem_vendor")] public ushort SubsystemVendor;
    [Native("subsystem_id")] public ushort SubsystemId;
    [Native("irq_line")] public byte IrqLine;
    /// <summary>Legacy INTx pin: 0 none, 1–4 = INTA–INTD.</summary>
    [Native("irq_pin")] public byte IrqPin;
    /// <summary>Physical address (memory) or port (I/O) of each BAR; 0 = unused.</summary>
    [Native("bar_address")] public fixed ulong BarAddress[6];
    [Native("bar_size")] public fixed ulong BarSize[6];
    /// <summary><see cref="Pci.BarIo"/>, <see cref="Pci.Bar64"/>, <see cref="Pci.BarPrefetchable"/>.</summary>
    [Native("bar_flags")] public fixed byte BarFlags[6];
}

/// <summary>
/// PCI / PCIe devices (runtime/src/arch/x86_64/pci.cpp): every bus is scanned at boot through ECAM (ACPI MCFG)
/// or the legacy ports. Devices are addressed by their index in that table.
/// </summary>
public static unsafe class Pci
{
    public const byte BarIo = 1, Bar64 = 2, BarPrefetchable = 4;

    /// <summary>Capability ids for <see cref="FindCapability"/>.</summary>
    public const byte CapMsi = 0x05, CapVendor = 0x09, CapExpress = 0x10, CapMsiX = 0x11;

    [Native("::neko::pci::count")]
    public static extern uint Count();

    [Native("::neko::pci::uses_ecam")]
    public static extern bool UsesEcam();

    [Native("::neko::pci::info")]
    public static extern bool GetInfo(uint index, out PciDevice device);

    /// <summary>Index of the next device with these ids at or after <paramref name="start"/>; -1 if none. 0xFFFF matches any id.</summary>
    [Native("::neko::pci::find")]
    public static extern int Find(ushort vendorId, ushort deviceId, uint start = 0);

    /// <summary>Index of the next device of this class (0xFF = any subclass); -1 if none.</summary>
    [Native("::neko::pci::find_class")]
    public static extern int FindClass(byte classCode, byte subclass, uint start = 0);

    [Native("::neko::pci::read32")]
    public static extern uint Read32(uint index, ushort offset);

    [Native("::neko::pci::read16")]
    public static extern ushort Read16(uint index, ushort offset);

    [Native("::neko::pci::read8")]
    public static extern byte Read8(uint index, ushort offset);

    [Native("::neko::pci::write32")]
    public static extern void Write32(uint index, ushort offset, uint value);

    [Native("::neko::pci::write16")]
    public static extern void Write16(uint index, ushort offset, ushort value);

    /// <summary>Turns on memory / I/O decoding and bus mastering (needed for DMA).</summary>
    [Native("::neko::pci::enable")]
    public static extern void Enable(uint index, bool memory, bool io, bool busMaster);

    /// <summary>Offset of capability <paramref name="id"/> in the configuration space, 0 if absent.</summary>
    [Native("::neko::pci::find_capability")]
    public static extern byte FindCapability(uint index, byte id, byte after = 0);

    /// <summary>Maps memory BAR <paramref name="bar"/> uncached; null for I/O or unused BARs.</summary>
    [Native("::neko::pci::map_bar")]
    public static extern void* MapBar(uint index, uint bar);

    /// <summary>Human-readable name of a class/subclass pair (PCI Code and ID Assignment Specification).</summary>
    public static string ClassName(byte classCode, byte subclass, byte progIf)
    {
        switch (classCode)
        {
            case 0x00: return "unclassified device";
            case 0x01:
                switch (subclass)
                {
                    case 0x00: return "SCSI controller";
                    case 0x01: return "IDE controller";
                    case 0x05: return "ATA controller";
                    case 0x06: return progIf == 0x01 ? "SATA controller (AHCI)" : "SATA controller";
                    case 0x07: return "SAS controller";
                    case 0x08: return progIf == 0x02 ? "NVMe controller" : "non-volatile memory controller";
                    default: return "storage controller";
                }
            case 0x02: return subclass == 0x00 ? "Ethernet controller" : "network controller";
            case 0x03: return subclass == 0x00 ? "VGA-compatible controller" : "display controller";
            case 0x04: return subclass == 0x03 ? "audio device (HD Audio)" : "multimedia controller";
            case 0x05: return "memory controller";
            case 0x06:
                switch (subclass)
                {
                    case 0x00: return "host bridge";
                    case 0x01: return "ISA bridge";
                    case 0x04: return "PCI-to-PCI bridge";
                    case 0x80: return "bridge";
                    default: return "bridge device";
                }
            case 0x07: return "communication controller";
            case 0x08: return "system peripheral";
            case 0x09: return "input device controller";
            case 0x0C:
                switch (subclass)
                {
                    case 0x03:
                        if (progIf == 0x30) return "USB controller (xHCI)";
                        if (progIf == 0x20) return "USB controller (EHCI)";
                        if (progIf == 0x10) return "USB controller (OHCI)";
                        if (progIf == 0x00) return "USB controller (UHCI)";
                        return "USB controller";
                    case 0x05: return "SMBus controller";
                    default: return "serial bus controller";
                }
            case 0x0D: return "wireless controller";
            default: return "device";
        }
    }

    /// <summary>A few well-known vendors; null otherwise.</summary>
    public static string VendorName(ushort vendorId)
    {
        switch (vendorId)
        {
            case 0x8086: return "Intel";
            case 0x1022: return "AMD";
            case 0x10DE: return "NVIDIA";
            case 0x1AF4: return "Red Hat (virtio)";
            case 0x1B36: return "Red Hat (QEMU)";
            case 0x1234: return "QEMU";
            case 0x15AD: return "VMware";
            case 0x80EE: return "VirtualBox";
            case 0x10EC: return "Realtek";
            case 0x14E4: return "Broadcom";
            case 0x144D: return "Samsung";
            default: return null;
        }
    }
}
