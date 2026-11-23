// mv FROM TO — renames or moves a file or directory (TO may be a directory to move into); files also across volumes.
using Nekotia;
using Nekotia.User;

namespace MvProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        if (argc != 3)
        {
            Console.WriteLine("usage: mv FROM TO");
            return 2;
        }
        byte* to = argv[2];
        byte* joined = stackalloc byte[File.MaxPath];
        if (File.IsDirectory(to))
        {
            Path.Join(to, Path.BaseName(argv[1]), joined, File.MaxPath);
            to = joined;
        }
        long e = File.Rename(argv[1], to);
        if (e == Process.ErrNotSupported && !File.IsDirectory(argv[1]))
        {
            // Another volume (/tmp ↔ /mnt): copy, then remove the original.
            e = Tool.CopyFile(argv[1], to);
            if (e == 0)
                e = File.Delete(argv[1]);
        }
        return e < 0 ? Tool.Fail("mv", argv[1], e) : 0;
    }
}
