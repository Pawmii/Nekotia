namespace Nekotia.Hal;

/// <summary>A USB device on the xHCI root ports. Mirrors <c>neko::usb::Info</c>.</summary>
[Native("::neko::usb::Info")]
public unsafe struct UsbDevice
{
    [Native("slot")] public byte Slot;
    [Native("port")] public byte Port;
    /// <summary>1 full (12 Mbit/s), 2 low (1.5), 3 high (480), 4 super (5 Gbit/s).</summary>
    [Native("speed")] public byte Speed;
    /// <summary>0 other, 1 keyboard, 2 mouse, 3 hub, 4 storage.</summary>
    [Native("kind")] public byte Kind;
    [Native("vendor_id")] public ushort VendorId;
    [Native("product_id")] public ushort ProductId;
    [Native("class_code")] public byte Class;
    [Native("subclass")] public byte Subclass;
    [Native("protocol")] public byte Protocol;
    [Native("product")] public fixed byte Product[40];
    [Native("reports")] public ulong Reports;
    /// <summary>Slot of the hub it hangs on, 0 = a root port (then Port is the root port).</summary>
    [Native("parent")] public byte Parent;
    [Native("depth")] public byte Depth;
    [Native("hub_ports")] public byte HubPorts;
    /// <summary>Storage: its block device ("usb0").</summary>
    [Native("disk")] public fixed byte Disk[8];
}

/// <summary>Mirrors <c>neko::usb::Stats</c>.</summary>
[Native("::neko::usb::Stats")]
public struct UsbStats
{
    [Native("attached")] public ulong Attached;
    [Native("detached")] public ulong Detached;
}

/// <summary>Mirrors <c>neko::usb::Mouse</c>: movement summed since boot.</summary>
[Native("::neko::usb::Mouse")]
public struct UsbMouse
{
    [Native("x")] public long X;
    [Native("y")] public long Y;
    [Native("wheel")] public long Wheel;
    [Native("buttons")] public byte Buttons;
    [Native("reports")] public ulong Reports;
}

/// <summary>USB (runtime/src/drivers/xhci.cpp): devices, HID keyboards (as PS/2 scancodes) and mice.</summary>
public static unsafe class Usb
{
    [Native("::neko::usb::count")]
    public static extern uint Count();

    [Native("::neko::usb::info")]
    public static extern bool GetInfo(uint index, out UsbDevice device);

    /// <summary>Where USB keyboard scancodes (set 1) go; called in interrupt context.</summary>
    [Native("::neko::usb::set_keyboard_sink")]
    public static extern void SetKeyboardSink(delegate*<byte, void> sink);

    [Native("::neko::usb::mouse")]
    public static extern UsbMouse Mouse();

    [Native("::neko::usb::stats")]
    public static extern UsbStats Stats();
}
