// echo ARGS… — prints its arguments separated by spaces.
using Nekotia.User;

namespace EchoProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        for (int i = 1; i < argc; i++)
        {
            if (i > 1)
                Console.WriteChar((byte)' ');
            Console.Write(argv[i]);
        }
        Console.WriteLine();
        return 0;
    }
}
