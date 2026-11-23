// host NAME… — looks names up in DNS (the server DHCP gave us; QEMU forwards to the host's resolver).
using Nekotia;
using Nekotia.User;

namespace HostProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        if (argc < 2)
        {
            Console.WriteLine("usage: host NAME…   (e.g. host example.com)");
            return 2;
        }
        int status = 0;
        for (int i = 1; i < argc; i++)
        {
            uint ip = 0;
            long e = Net.Resolve(argv[i], out ip);
            if (e < 0)
            {
                status = Tool.Fail("host", argv[i], e);
                continue;
            }
            Console.Write(argv[i]);
            Console.Write(" has address ");
            Net.WriteIp(ip);
            Console.WriteLine();
        }
        return status;
    }
}
