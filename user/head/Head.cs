// head [-n N] [FILE] — the first N lines (default 10) of a file or stdin.
using Nekotia;
using Nekotia.User;

namespace HeadProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        long count = 10;
        int i = 1;
        if (i + 1 < argc && Str.Equals(argv[i], "-n"))
        {
            count = 0;
            for (byte* p = argv[i + 1]; *p >= '0' && *p <= '9'; p++)
                count = count * 10 + (*p - '0');
            i += 2;
        }
        long fd = Tool.OpenInput(argc, argv, i);
        if (fd < 0)
            return Tool.Fail("head", argv[i], fd);
        LineReader reader = default;
        reader.Open((int)fd);
        byte* line = (byte*)Memory.Alloc(LineReader.Size);
        for (long n = 0; n < count && reader.Next(line, LineReader.Size) >= 0; n++)
        {
            Console.Write(line);
            Console.WriteLine();
        }
        return 0;   // closing stdin here is what tells a writer upstream to stop (broken pipe)
    }
}
