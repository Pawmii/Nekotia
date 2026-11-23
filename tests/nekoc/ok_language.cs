// Valid code: must transpile without diagnostics and the C++ must compile (tests/nekoc/run.py).
using Nekotia;
using Nekotia.User;

namespace NekocTests;

public enum Color : byte { Red = 1, Green = 2, Blue = 3 }

public struct Pair
{
    public int A;
    public int B;
    public int Sum => A + B;
}

public static unsafe class Program
{
    static int Classify(int v) => v switch
    {
        < 0 => -1,
        0 => 0,
        >= 1 and <= 9 => 1,
        10 or 20 => 2,
        _ when v % 2 == 0 => 3,
        _ => 4,
    };

    static string Name(Color c) => c switch
    {
        Color.Red => "red",
        Color.Green => "green",
        _ => "other",
    };

    static int Count(byte* s)
    {
        int letters = 0;
        for (; *s != 0; s++)
        {
            switch (*s)
            {
                case >= (byte)'a' and <= (byte)'z':
                case >= (byte)'A' and <= (byte)'Z':
                    letters++;
                    break;
                case (byte)' ':
                    continue;
                default:
                    break;
            }
        }
        return letters;
    }

    static int Guarded(int v)
    {
        int seen = 0;
        byte* p = null;
        try {
            seen = v;
            p = Str.Bytes("set");
            if (v > 3)
                throw new System.InvalidOperationException("big");
        } catch (System.InvalidOperationException e) when (e.Message != null) {
            return seen + (p != null ? 100 : 0);
        } catch (System.Exception) {
            throw;
        } finally {
            seen = -1;
        }
        return seen;
    }

    public static int Main()
    {
        Guarded(5);
        Pair p = new Pair { A = 2, B = 3 };
        byte* buffer = stackalloc byte[64];
        var t = new Text(buffer, 64);
        t.Write($"{p.Sum,4}|{Name(Color.Green),-6}|{Str.Bytes("x")}|{255:X4}|{3:D2}|{1.25:F1}");
        Console.WriteLine($"{buffer} {Classify(7)} {Count(Str.Bytes("ab c"))}");
        char c = 'k';
        return c is >= 'a' and <= 'z' && p.A is not 0 ? 0 : 1;
    }
}
