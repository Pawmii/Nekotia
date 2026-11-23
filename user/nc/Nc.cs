// nc HOST PORT   — connects; stdin goes to the socket, the socket to stdout (Ctrl+D ends sending)
// nc -l PORT     — waits for one connection, then the same
using Nekotia;
using Nekotia.User;

namespace NcProgram;

public static unsafe class Program
{
    static int _socket;

    static void SocketToStdout(ulong unused)
    {
        byte* buffer = (byte*)Memory.Alloc(4096);
        while (true)
        {
            long n = File.ReadBytes(_socket, buffer, 4096);
            if (n <= 0)
                break;
            Net.WriteAll(1, buffer, n);
        }
        Memory.Free(buffer);
    }

    public static int Main(int argc, byte** argv)
    {
        long fd;
        if (argc == 3 && Str.Equals(argv[1], "-l"))
        {
            long listener = Net.Listen((ushort)Number(argv[2]));
            if (listener < 0)
                return Tool.Fail("nc", argv[2], listener);
            uint* peer = stackalloc uint[2];
            fd = Net.Accept((int)listener, peer);
            File.Close((int)listener);
            if (fd < 0)
                return Tool.Fail("nc", null, fd);
            Console.Write(Ansi.Gray);
            Console.Write("connection from ");
            Net.WriteIp(peer[0]);
            Console.WriteLine($":{peer[1]}{Ansi.Reset}");
        }
        else if (argc == 3)
        {
            uint ip = 0;
            long e = Net.Resolve(argv[1], out ip);
            if (e < 0)
                return Tool.Fail("nc", argv[1], e);
            fd = Net.Connect(ip, (ushort)Number(argv[2]));
            if (fd < 0)
                return Tool.Fail("nc", argv[1], fd);
        }
        else
        {
            Console.WriteLine("usage: nc HOST PORT  |  nc -l PORT");
            return 2;
        }
        _socket = (int)fd;
        ulong reader = Thread.Start(&SocketToStdout);
        byte* buffer = (byte*)Memory.Alloc(4096);
        while (true)
        {
            long n = File.ReadBytes(0, buffer, 4096);
            if (n <= 0)
                break;
            if (!Net.WriteAll(_socket, buffer, n))
                break;
        }
        Net.Shutdown(_socket);   // end of our input: the peer sees end of stream
        Thread.Join(reader);
        return 0;
    }

    static long Number(byte* s)
    {
        long v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        return v;
    }
}
