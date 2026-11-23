// cat [FILE…] — copies files (or, without arguments, its standard input) to standard output.
using Nekotia;
using Nekotia.User;

namespace CatProgram;

public static unsafe class Program
{
    const int BufferSize = 4096;

    public static int Main(int argc, byte** argv)
    {
        byte* buffer = (byte*)Memory.Alloc(BufferSize);
        if (argc < 2)
            return Copy(0, buffer, Str.Bytes("stdin"));
        int status = 0;
        for (int i = 1; i < argc; i++)
        {
            long fd = File.Open(argv[i], File.Read);
            if (fd < 0)
            {
                status = Fail(argv[i], fd);
                continue;
            }
            status |= Copy((int)fd, buffer, argv[i]);
            File.Close((int)fd);
        }
        return status;
    }

    static int Copy(int fd, byte* buffer, byte* name)
    {
        while (true)
        {
            long n = File.ReadBytes(fd, buffer, BufferSize);
            if (n == 0)
                return 0;
            if (n < 0)
                return Fail(name, n);
            long put = File.WriteBytes(1, buffer, (ulong)n);
            if (put < 0)
                return Fail(Str.Bytes("stdout"), put);
        }
    }

    static int Fail(byte* name, long error)
    {
        Console.Write(Ansi.Red);
        Console.Write("cat: ");
        Console.Write(name);
        Console.Write(": ");
        Console.Write(Process.ErrorText(error));
        Console.WriteLine(Ansi.Reset);
        return 1;
    }
}
