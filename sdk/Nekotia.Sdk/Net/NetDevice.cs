namespace Nekotia.Net;

/// <summary>A network interface. Mirrors <c>neko::net::Info</c>.</summary>
[Native("::neko::net::Info")]
public unsafe struct NetInfo
{
    [Native("name")] public fixed byte Name[8];
    [Native("mac")] public fixed byte Mac[6];
    [Native("mtu")] public ushort Mtu;
    [Native("rx_packets")] public ulong RxPackets;
    [Native("tx_packets")] public ulong TxPackets;
    [Native("rx_bytes")] public ulong RxBytes;
    [Native("tx_bytes")] public ulong TxBytes;
    [Native("rx_dropped")] public ulong RxDropped;
}

/// <summary>
/// Ethernet interfaces (runtime/src/core/netif.cpp; driver: virtio-net). The protocol stack on top of them is
/// C# — kernel/Net/.
/// </summary>
public static unsafe class NetDevice
{
    public const int MaxFrame = 1514;

    [Native("::neko::net::count")]
    public static extern uint Count();

    [Native("::neko::net::info")]
    public static extern bool GetInfo(uint index, out NetInfo info);

    /// <summary>Sends one Ethernet frame (14-byte header included, no FCS); 0 or an error.</summary>
    [Native("::neko::net::send")]
    public static extern long Send(uint index, void* frame, ulong length);

    /// <summary>Puts a frame into the interface's receive ring as if it had arrived (loopback).</summary>
    [Native("::neko::net::deliver")]
    public static extern void Inject(uint index, void* frame, ulong length);

    /// <summary>Waits for a frame (timeoutMs 0 = forever); its length, 0 on timeout, or an error.</summary>
    [Native("::neko::net::receive")]
    public static extern long Receive(uint index, void* buffer, ulong capacity, ulong timeoutMs = 0);
}
