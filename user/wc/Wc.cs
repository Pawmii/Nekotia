// wc [FILE…] — lines, words and bytes of files or stdin.
using Nekotia;
using Nekotia.User;

namespace WcProgram;

public static unsafe class Program
{
    static ulong _lines, _words, _bytes;

    public static int Main(int argc, byte** argv)
    {
        byte* buffer = (byte*)Memory.Alloc(4096);
        if (argc < 2)
        {
            Count(0, buffer);
            Report(_lines, _words, _bytes, null);
            return 0;
        }
        ulong tl = 0, tw = 0, tb = 0;
        int status = 0;
        for (int i = 1; i < argc; i++)
        {
            long fd = File.Open(argv[i], File.Read);
            if (fd < 0)
            {
                status = Tool.Fail("wc", argv[i], fd);
                continue;
            }
            _lines = _words = _bytes = 0;
            Count((int)fd, buffer);
            File.Close((int)fd);
            Report(_lines, _words, _bytes, argv[i]);
            tl += _lines;
            tw += _words;
            tb += _bytes;
        }
        if (argc > 2)
            Report(tl, tw, tb, Str.Bytes("total"));
        return status;
    }

    static void Count(int fd, byte* buffer)
    {
        bool inWord = false;
        while (true)
        {
            long n = File.ReadBytes(fd, buffer, 4096);
            if (n <= 0)
                return;
            _bytes += (ulong)n;
            for (long i = 0; i < n; i++)
            {
                byte c = buffer[i];
                if (c == '\n')
                    _lines++;
                bool space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
                if (!space && !inWord)
                    _words++;
                inWord = !space;
            }
        }
    }

    static void Column(ulong v)
    {
        int digits = 1;
        for (ulong x = v; x >= 10; x /= 10)
            digits++;
        for (int i = digits; i < 8; i++)
            Console.WriteChar((byte)' ');
        Console.WriteDec(v);
    }

    static void Report(ulong lines, ulong words, ulong bytes, byte* name)
    {
        Column(lines);
        Column(words);
        Column(bytes);
        if (name != null)
        {
            Console.Write(" ");
            Console.Write(name);
        }
        Console.WriteLine();
    }
}
