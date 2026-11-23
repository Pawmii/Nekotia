// ls [PATH…] — lists directories (names, then sizes); a file path prints just that file.
using Nekotia;
using Nekotia.User;

namespace LsProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        if (argc < 2)
            return List(Str.Bytes("."), false);
        int status = 0;
        for (int i = 1; i < argc; i++)
            status |= List(argv[i], argc > 2);
        return status;
    }

    static int List(byte* path, bool heading)
    {
        FileStat st = default;
        long e = File.Stat(path, out st);
        if (e < 0)
            return Fail(path, e);
        if (st.Type != (byte)NodeType.Directory)
        {
            Console.Write(path);
            Console.WriteLine($"  {st.Size:size}");
            return 0;
        }
        long fd = File.Open(path, File.Read);
        if (fd < 0)
            return Fail(path, fd);
        if (heading)
        {
            Console.Write(path);
            Console.WriteLine(":");
        }
        DirEntry entry = default;
        uint n = 0;
        for (uint i = 0; File.ReadDirectory((int)fd, i, out entry) == 1; i++)
        {
            int width = (int)Str.Length(entry.Name);
            if (entry.Type == (byte)NodeType.Directory)
            {
                Console.Write(Ansi.Blue);
                Console.Write(entry.Name);
                Console.Write("/");
                width++;
            }
            else if (entry.Type == (byte)NodeType.Device)
            {
                Console.Write(Ansi.Yellow);
                Console.Write(entry.Name);
            }
            else
            {
                Console.Write(entry.Name);
            }
            Console.Write(Ansi.Reset);
            for (int pad = width; pad < 20; pad++)
                Console.WriteChar((byte)' ');
            if (entry.Type == (byte)NodeType.Directory && entry.Size == 1)
                Console.WriteLine("1 entry");
            else if (entry.Type == (byte)NodeType.Directory)
                Console.WriteLine($"{entry.Size} entries");
            else if (entry.Type == (byte)NodeType.Device)
                Console.WriteLine("device");
            else
                Console.WriteLine($"{entry.Size:size}");
            n++;
        }
        File.Close((int)fd);
        if (n == 0)
            Console.WriteLine(Ansi.Gray, "(empty)");
        return 0;
    }

    static int Fail(byte* path, long error)
    {
        Console.Write(Ansi.Red);
        Console.Write("ls: ");
        Console.Write(path);
        Console.Write(": ");
        Console.Write(Process.ErrorText(error));
        Console.WriteLine(Ansi.Reset);
        return 1;
    }
}
