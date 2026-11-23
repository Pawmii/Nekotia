// Guess the number: reads the keyboard through the read system call (implemented in the kernel's C#).
using Nekotia;
using Nekotia.User;

namespace Guess;

public static unsafe class Program
{
    public static int Main()
    {
        ulong seed = Time.UptimeMs() * 6364136223846793005UL + 1442695040888963407UL;
        int secret = (int)((seed >> 33) % 100) + 1;
        byte* line = stackalloc byte[64];

        Console.WriteLine(Ansi.Cyan, "I am thinking of a number between 1 and 100.");
        for (int tries = 1; ; tries++)
        {
            Console.Write($"guess #{tries}: ");
            if (Console.ReadLine(line, 64) < 0)
            {
                Console.WriteLine();
                Console.WriteLine($"giving up? it was {secret}.");
                return 1;
            }
            int guess = Parse(line);
            if (guess < 1 || guess > 100)
                Console.WriteLine(Ansi.Yellow, "a number from 1 to 100, please");
            else if (guess < secret)
                Console.WriteLine("higher");
            else if (guess > secret)
                Console.WriteLine("lower");
            else
            {
                Console.Write(Ansi.Green);
                Console.Write($"yes! {secret} in {tries} tries");
                Console.WriteLine(Ansi.Reset);
                return 0;
            }
        }
    }

    /// Decimal number, -1 when the line is not one.
    static int Parse(byte* s)
    {
        while (*s == ' ')
            s++;
        int value = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 6)
        {
            value = value * 10 + (*s - '0');
            s++;
            digits++;
        }
        while (*s == ' ')
            s++;
        return digits > 0 && *s == 0 ? value : -1;
    }
}
