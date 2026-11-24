// play FILE.wav… — plays WAV files (PCM, 8 or 16 bits, mono or stereo, any rate: converted to 48 kHz stereo).
using Nekotia;
using Nekotia.User;

namespace PlayProgram;

public static unsafe class Program
{
    const int Chunk = 4096;   // frames per write

    public static int Main(int argc, byte** argv)
    {
        if (argc < 2) {
            Console.WriteLine("usage: play FILE.wav…   (try: play /usr/share/sounds/meow.wav)");
            return 2;
        }
        long audio = Audio.Open();
        if (audio < 0) {
            Console.Write("play: no sound device: ");
            Console.WriteLine(Process.ErrorText(audio));
            return 1;
        }
        int failed = 0;
        for (int i = 1; i < argc; i++) {
            if (PlayFile((int)audio, argv[i]) != 0)
                failed++;
        }
        Audio.Drain((int)audio);
        File.Close((int)audio);
        return failed == 0 ? 0 : 1;
    }

    static uint U32(byte* p) => (uint)p[0] | ((uint)p[1] << 8) | ((uint)p[2] << 16) | ((uint)p[3] << 24);
    static uint U16(byte* p) => (uint)p[0] | ((uint)p[1] << 8);

    static int Fail(byte* path, string why)
    {
        Console.Write("play: ");
        Console.Write(path);
        Console.Write(": ");
        Console.WriteLine(why);
        return 1;
    }

    static int PlayFile(int audio, byte* path)
    {
        long fd = File.Open(path, File.Read);
        if (fd < 0)
            return Fail(path, Process.ErrorText(fd));
        FileStat st = default;
        File.Stat((int)fd, out st);
        if (st.Size < 44 || st.Size > (64ul << 20)) {
            File.Close((int)fd);
            return Fail(path, "not a WAV file (or larger than 64 MiB)");
        }
        byte* data = (byte*)Memory.Alloc(st.Size);
        long got = 0;
        while ((ulong)got < st.Size) {
            long n = File.ReadBytes((int)fd, data + got, st.Size - (ulong)got);
            if (n <= 0)
                break;
            got += n;
        }
        File.Close((int)fd);
        int r = Decode(audio, path, data, got);
        Memory.Free(data);
        return r;
    }

    static int Decode(int audio, byte* path, byte* data, long size)
    {
        if (U32(data) != 0x46464952 || U32(data + 8) != 0x45564157)   // "RIFF" … "WAVE"
            return Fail(path, "not a WAV file");
        uint format = 0, channels = 0, rate = 0, bits = 0;
        byte* samples = null;
        long length = 0;
        long at = 12;
        while (at + 8 <= size) {
            uint id = U32(data + at), chunk = U32(data + at + 4);
            byte* body = data + at + 8;
            if (id == 0x20746D66 && chunk >= 16) {   // "fmt "
                format = U16(body);
                channels = U16(body + 2);
                rate = U32(body + 4);
                bits = U16(body + 14);
            } else if (id == 0x61746164) {           // "data"
                samples = body;
                length = (long)chunk;
                if (at + 8 + length > size)
                    length = size - at - 8;
            }
            at += 8 + (long)chunk + (long)(chunk & 1);
        }
        if (samples == null || format != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2 || rate < 1000 || rate > 192000)
            return Fail(path, "unsupported WAV (PCM, 8/16 bits, 1-2 channels only)");
        uint frameBytes = channels * bits / 8;
        long frames = length / frameBytes;
        Console.WriteLine($"{path}: {rate} Hz, {bits}-bit, {(channels == 1 ? "mono" : "stereo")}, {frames / rate}.{frames % rate * 10 / rate} s");

        short* output = (short*)Memory.Alloc(Chunk * 4);
        // Linear interpolation: the position in the input advances by rate / 48000 per output frame (32.32 fixed point).
        ulong step = ((ulong)rate << 32) / Audio.Rate;
        ulong position = 0;
        int n = 0;
        while (true) {
            long index = (long)(position >> 32);
            if (index >= frames)
                break;
            long next = index + 1 < frames ? index + 1 : index;
            int fraction = (int)((position >> 16) & 0xFFFF);
            for (uint c = 0; c < 2; c++) {
                uint source = c < channels ? c : 0;
                int a = Sample(samples, index, source, frameBytes, bits), b = Sample(samples, next, source, frameBytes, bits);
                output[n * 2 + (int)c] = (short)(a + (((b - a) * fraction) >> 16));
            }
            n++;
            position += step;
            if (n == Chunk) {
                if (File.WriteBytes(audio, output, (ulong)n * 4) < 0)
                    break;
                n = 0;
            }
        }
        if (n > 0)
            File.WriteBytes(audio, output, (ulong)n * 4);
        Memory.Free(output);
        return 0;
    }

    static int Sample(byte* samples, long frame, uint channel, uint frameBytes, uint bits)
    {
        byte* p = samples + frame * frameBytes + channel * (bits / 8);
        if (bits == 8)
            return ((int)p[0] - 128) << 8;
        return (short)(p[0] | (p[1] << 8));
    }
}
