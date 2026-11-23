// ls — the programs in /boot/bin (there is no file system yet; see ROADMAP phase 6).
using Nekotia;
using Nekotia.User;

namespace LsProgram;

public static unsafe class Program
{
    public static int Main()
    {
        ProgramInfo info;
        uint n = 0;
        for (uint i = 0; Process.GetProgram(i, out info); i++)
        {
            Console.Write(Ansi.Cyan);
            Console.Write(info.Name);
            Console.Write(Ansi.Reset);
            for (int pad = (int)Str.Length(info.Name); pad < 14; pad++)
                Console.WriteChar((byte)' ');
            Console.WriteLine($"{info.Size:size}");
            n++;
        }
        Console.WriteLine($"{n} program(s) in /boot/bin");
        return 0;
    }
}
