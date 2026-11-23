// cp FROM TO — copies a file (TO may be a directory to copy into); works across volumes (/tmp ↔ /mnt).
using Nekotia;
using Nekotia.User;

namespace CpProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        if (argc != 3)
        {
            Console.WriteLine("usage: cp FROM TO");
            return 2;
        }
        if (File.IsDirectory(argv[1]))
            return Tool.Fail("cp", argv[1], Process.ErrIsDirectory);
        byte* to = argv[2];
        byte* joined = stackalloc byte[File.MaxPath];
        if (File.IsDirectory(to))
        {
            Path.Join(to, Path.BaseName(argv[1]), joined, File.MaxPath);
            to = joined;
        }
        long e = Tool.CopyFile(argv[1], to);
        return e < 0 ? Tool.Fail("cp", argv[1], e) : 0;
    }
}
