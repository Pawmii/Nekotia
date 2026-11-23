// rm PATH… — removes files and empty directories.
using Nekotia;
using Nekotia.User;

namespace RmProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        if (argc < 2)
        {
            Console.WriteLine("usage: rm PATH…   (files and empty directories)");
            return 2;
        }
        int status = 0;
        for (int i = 1; i < argc; i++)
        {
            long e = File.Delete(argv[i]);
            if (e < 0)
                status = Tool.Fail("rm", argv[i], e);
        }
        return status;
    }
}
