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
}
