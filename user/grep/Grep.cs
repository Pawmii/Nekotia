// grep [-v] [-i] [-c] PATTERN [FILE…] — lines containing PATTERN (a plain substring), from files or stdin.
using Nekotia;
using Nekotia.User;

namespace GrepProgram;

public static unsafe class Program
{
    static bool _invert, _ignoreCase, _count;

    public static int Main(int argc, byte** argv)
    {
        int i = 1;
        while (i < argc && argv[i][0] == '-' && argv[i][1] != 0)
        {
            for (byte* f = argv[i] + 1; *f != 0; f++)
            {
                if (*f == 'v') _invert = true;
                else if (*f == 'i') _ignoreCase = true;
                else if (*f == 'c') _count = true;
                else return Usage();
            }
            i++;
        }
        if (i >= argc)
            return Usage();
        byte* pattern = argv[i++];
        long matches = 0;
        int status = 0;
        if (i >= argc)
            matches += Search(0, pattern, null);
        for (; i < argc; i++)
        {
            long fd = File.Open(argv[i], File.Read);
            if (fd < 0)
            {
                status = Tool.Fail("grep", argv[i], fd);
                continue;
            }
            matches += Search((int)fd, pattern, argc - i > 1 || i > 2 + (_invert || _ignoreCase || _count ? 1 : 0) ? argv[i] : null);
            File.Close((int)fd);
        }
        if (_count)
            Console.WriteLine($"{matches}");
        return status != 0 ? 2 : matches > 0 ? 0 : 1;
    }

    static int Usage()
    {
        Console.WriteLine("usage: grep [-v] [-i] [-c] PATTERN [FILE…]   (-v: non-matching, -i: any case, -c: count)");
        return 2;
    }

    static byte Lower(byte c) => c >= 'A' && c <= 'Z' ? (byte)(c + 32) : c;

    static bool Contains(byte* line, byte* pattern)
    {
        if (*pattern == 0)
            return true;
        for (byte* s = line; *s != 0; s++)
        {
            int k = 0;
            while (pattern[k] != 0 && s[k] != 0
                   && (_ignoreCase ? Lower(s[k]) == Lower(pattern[k]) : s[k] == pattern[k]))
                k++;
            if (pattern[k] == 0)
                return true;
        }
        return false;
    }

    static long Search(int fd, byte* pattern, byte* name)
    {
        LineReader reader = default;
        reader.Open(fd);
        byte* line = (byte*)Memory.Alloc(LineReader.Size);
        long matches = 0;
        while (reader.Next(line, LineReader.Size) >= 0)
        {
            if (Contains(line, pattern) == _invert)
                continue;
            matches++;
            if (_count)
                continue;
            if (name != null)
            {
                Console.Write(Ansi.Magenta);
                Console.Write(name);
                Console.Write(Ansi.Reset);
                Console.Write(":");
            }
            Console.Write(line);
            Console.WriteLine();
        }
        Memory.Free(line);
        reader.Close();
        return matches;
    }
}
