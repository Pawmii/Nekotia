// volume [0-100] — shows or sets the master volume of the sound device.
using Nekotia;
using Nekotia.User;

namespace VolumeProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        long fd = Audio.Open();
        if (fd < 0) {
            Console.Write("volume: no sound device: ");
            Console.WriteLine(Process.ErrorText(fd));
            return 1;
        }
        if (argc > 1) {
            long v = 0;
            for (byte* p = argv[1]; *p != 0; p++) {
                if (*p < '0' || *p > '9' || v > 100) {
                    v = 101;
                    break;
                }
                v = v * 10 + (*p - '0');
            }
            if (v > 100) {
                Console.WriteLine("usage: volume [0-100]");
                return 2;
            }
            Audio.SetVolume((int)fd, (uint)v);
        }
        Console.WriteLine($"volume {Audio.Volume((int)fd)}%");
        File.Close((int)fd);
        return 0;
    }
}
