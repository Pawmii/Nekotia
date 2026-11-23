// Each `// error CODE` marks a line on which nekoc must report exactly that diagnostic.
using Nekotia;
using Nekotia.User;

namespace NekocTests;

public class Reference { }   // error NEKO0100

public static unsafe class Program
{
    public static int Main()
    {
        int x = 3;
        string s = $"x = {x}";                        // error NEKO0300
        bool b = x is int y;                          // error NEKO0300
        System.Func<int> f = () => 1;                 // error NEKO0201 NEKO0300
        Console.WriteLine(System.Math.Abs(x).ToString());   // error NEKO0200
        int z = x switch { 1 => 1, _ => 0 };
        return z;
    }
}
