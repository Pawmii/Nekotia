// mkdir DIR… — creates directories.
using Nekotia;
using Nekotia.User;

namespace MkdirProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        if (argc < 2)
        {
            Console.WriteLine("usage: mkdir DIR…");
            return 2;
        }
        int status = 0;
        for (int i = 1; i < argc; i++)
        {
            long e = File.CreateDirectory(argv[i]);
            if (e < 0)
                status = Tool.Fail("mkdir", argv[i], e);
        }
        return status;
    }
}
