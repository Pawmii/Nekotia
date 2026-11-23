namespace Nekotia.User;

/// <summary>Path strings (NUL-terminated UTF-8, '/'-separated).</summary>
public static unsafe class Path
{
    /// <summary>The part after the last '/' (points into <paramref name="path"/>).</summary>
    public static byte* BaseName(byte* path)
    {
        byte* name = path;
        for (byte* c = path; *c != 0; c++)
        {
            if (*c == '/' && c[1] != 0)
                name = c + 1;
        }
        return name;
    }

    /// <summary><paramref name="directory"/> + "/" + <paramref name="name"/> into <paramref name="output"/> (truncated to fit).</summary>
    public static void Join(byte* directory, byte* name, byte* output, int capacity)
    {
        int n = 0;
        for (byte* d = directory; *d != 0 && n < capacity - 2; d++)
            output[n++] = *d;
        if (n > 0 && output[n - 1] != '/')
            output[n++] = (byte)'/';
        for (byte* c = name; *c != 0 && *c != '/' && n < capacity - 1; c++)
            output[n++] = *c;
        output[n] = 0;
    }
}

/// <summary>Helpers shared by the command-line tools in user/.</summary>
public static unsafe class Tool
{
    /// <summary>Prints "TOOL: PATH: error text" in red and returns exit code 1.</summary>
    public static int Fail(string tool, byte* path, long error)
    {
        Console.Write(Ansi.Red);
        Console.Write(tool);
        Console.Write(": ");
        if (path != null)
        {
            Console.Write(path);
            Console.Write(": ");
        }
        Console.Write(Process.ErrorText(error));
        Console.WriteLine(Ansi.Reset);
        return 1;
    }
    /// <summary>Copies the file <paramref name="from"/> to <paramref name="to"/> (created or replaced). 0 or an error.</summary>
    public static long CopyFile(byte* from, byte* to)
    {
        long src = File.Open(from, File.Read);
        if (src < 0)
            return src;
        long dst = File.Open(to, File.Write | File.Create | File.Truncate);
        if (dst < 0)
        {
            File.Close((int)src);
            return dst;
        }
        const int Size = 16 * 1024;
        byte* buffer = (byte*)Memory.Alloc(Size);
        long result = buffer == null ? Process.ErrNoMemory : 0;
        while (result == 0)
        {
            long n = File.ReadBytes((int)src, buffer, Size);
            if (n <= 0)
            {
                result = n;
                break;
            }
            long put = File.WriteBytes((int)dst, buffer, (ulong)n);
            if (put != n)
                result = put < 0 ? put : Process.ErrNoMemory;
        }
        Memory.Free(buffer);
        File.Close((int)src);
        File.Close((int)dst);
        return result;
    }

    /// <summary>Opens argv[index] for reading, or returns 0 (stdin) when there is no such argument or it is "-".</summary>
    public static long OpenInput(int argc, byte** argv, int index)
    {
        if (index >= argc || Str.Equals(argv[index], "-"))
            return 0;
        return File.Open(argv[index], File.Read);
    }
}

/// <summary>Reads a file descriptor line by line (lines longer than the buffer are split).</summary>
public unsafe struct LineReader
{
    int _fd;
    byte* _buffer;
    int _length;
    int _position;
    bool _end;

    public const int Size = 4096;

    public void Open(int fd)
    {
        _fd = fd;
        _buffer = (byte*)Memory.Alloc(Size);
        _length = 0;
        _position = 0;
        _end = false;
    }

    public void Close() => Memory.Free(_buffer);

    /// <summary>Copies the next line (without '\n', NUL-terminated) into <paramref name="line"/>; its length, or -1 at the end.</summary>
    public long Next(byte* line, int capacity)
    {
        int n = 0;
        bool any = false;
        while (true)
        {
            if (_position == _length)
            {
                if (_end)
                    break;
                long got = File.ReadBytes(_fd, _buffer, Size);
                if (got <= 0)
                {
                    _end = true;
                    break;
                }
                _length = (int)got;
                _position = 0;
            }
            any = true;
            byte c = _buffer[_position++];
            if (c == '\n')
                break;
            if (n < capacity - 1)
                line[n++] = c;
        }
        line[n] = 0;
        return any ? n : -1;
    }
}
