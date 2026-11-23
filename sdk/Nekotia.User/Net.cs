namespace Nekotia.User;

/// <summary>
/// TCP/IP for programs (system calls 40–48; the stack is C# in the kernel, kernel/Net/). Sockets are file
/// descriptors: read, write, close and redirect them like files. Addresses are uints in host order
/// (10.0.2.15 = 0x0A00020F). Negative results are errors (<see cref="Process.ErrorText"/>).
/// </summary>
public static unsafe class Net
{
    /// <summary>Connects to ip:port; the socket fd, or ErrNotFound (refused, unreachable).</summary>
    [Native("::neko::user::tcp_connect")]
    public static extern long Connect(uint ip, ushort port);

    /// <summary>Listens on <paramref name="port"/>; accept connections with <see cref="Accept"/>.</summary>
    [Native("::neko::user::tcp_listen")]
    public static extern long Listen(ushort port);

    /// <summary>Waits for the next connection; its fd. <paramref name="peer"/> gets {ip, port} (may be null).</summary>
    [Native("::neko::user::tcp_accept")]
    public static extern long Accept(int fd, uint* peer);

    /// <summary>No more sending (FIN): the peer reads end of stream; reading goes on.</summary>
    [Native("::neko::user::tcp_shutdown")]
    public static extern long Shutdown(int fd);

    /// <summary>A UDP socket on <paramref name="port"/> (0: any free one).</summary>
    [Native("::neko::user::udp_open")]
    public static extern long UdpOpen(ushort port);

    [Native("::neko::user::udp_sendto")]
    public static extern long SendTo(int fd, uint ip, ushort port, void* data, ulong length);

    /// <summary>Next datagram (0 after <paramref name="timeoutMs"/>; 0 = wait forever). <paramref name="from"/> gets {ip, port}.</summary>
    [Native("::neko::user::udp_recvfrom")]
    public static extern long ReceiveFrom(int fd, void* buffer, ulong length, uint* from, ulong timeoutMs = 0);

    /// <summary>Host name (DNS), "a.b.c.d" or "localhost" → address. 0 or ErrNotFound.</summary>
    [Native("::neko::user::resolve")]
    public static extern long Resolve(byte* name, out uint ip);

    [Native("::neko::user::resolve")]
    public static extern long Resolve(string name, out uint ip);

    /// <summary>{address, netmask, gateway, DNS}; all 0 when there is no network.</summary>
    [Native("::neko::user::net_config")]
    public static extern long Config(uint* four);

    /// <summary>Writes a.b.c.d.</summary>
    public static void WriteIp(uint ip)
    {
        Console.WriteDec((ulong)(ip >> 24));
        Console.WriteChar((byte)'.');
        Console.WriteDec((ulong)((ip >> 16) & 255));
        Console.WriteChar((byte)'.');
        Console.WriteDec((ulong)((ip >> 8) & 255));
        Console.WriteChar((byte)'.');
        Console.WriteDec((ulong)(ip & 255));
    }

    /// <summary>Writes all of <paramref name="length"/> bytes to fd (sockets may take less per call).</summary>
    public static bool WriteAll(int fd, byte* data, long length)
    {
        while (length > 0)
        {
            long n = File.WriteBytes(fd, data, (ulong)length);
            if (n <= 0)
                return false;
            data += n;
            length -= n;
        }
        return true;
    }

    public static bool WriteAll(int fd, string text) => WriteAll(fd, Str.Bytes(text), (long)Str.Length(text));
}
