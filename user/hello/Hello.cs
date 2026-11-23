// The smallest tour of user mode: arguments, system calls, the heap and floating point — all in ring 3.
using Nekotia;
using Nekotia.User;

namespace Hello;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        Console.WriteLine(Ansi.Cyan, "Hello from ring 3!");
        Console.WriteLine($"I am a C# program running as process {Process.Id()}, {Time.UptimeMs()} ms after boot.");

        Console.Write($"arguments: {argc - 1}");
        for (int i = 1; i < argc; i++)
        {
            Console.Write(i == 1 ? " — " : ", ");
            Console.Write(argv[i]);
        }
        Console.WriteLine();

        ulong* squares = Memory.NewArray<ulong>(10000);
        ulong sum = 0;
        for (ulong i = 0; i < 10000; i++)
        {
            squares[i] = i * i;
            sum += squares[i];
        }
        Console.WriteLine($"sum of 10000 squares: {sum} (my heap: {Memory.TotalBytes():size})");
        Memory.Free(squares);

        Console.WriteLine($"sqrt(2) = {Maths.Sqrt(2.0)}, e = {Maths.Exp(1.0)}");
        return 0;
    }
}
