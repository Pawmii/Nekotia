// primes [N] [THREADS] — counts the primes below N (default 2 000 000) with 1 thread, then with THREADS
// (default 4) threads sharing the work, and prints the speed-up: the threads really run on several CPUs.
using Nekotia;
using Nekotia.User;

namespace PrimesProgram;

public static unsafe class Program
{
    const int MaxThreads = 16;
    const long Chunk = 10000;

    static long _limit;
    static long* _next;     // next chunk to hand out (atomic)
    static long* _found;    // primes found so far (atomic)

    public static int Main(int argc, byte** argv)
    {
        _limit = argc > 1 ? Parse(argv[1], 2000000) : 2000000;
        long threads = argc > 2 ? Parse(argv[2], 4) : 4;
        if (threads < 1) threads = 1;
        if (threads > MaxThreads) threads = MaxThreads;
        _next = Memory.New<long>();
        _found = Memory.New<long>();

        ulong single = Run(1);
        long count = *_found;
        ulong parallel = Run((int)threads);
        Console.WriteLine($"{count} primes below {_limit}");
        Console.WriteLine($"1 thread: {single} ms, {threads} threads: {parallel} ms — x{(double)single / (parallel == 0 ? 1 : parallel):F2}");
        return *_found == count ? 0 : 1;
    }

    static ulong Run(int threads)
    {
        Atomic.Store(_next, 2);
        Atomic.Store(_found, 0);
        ulong start = Time.UptimeMs();
        ulong* handles = stackalloc ulong[MaxThreads];
        for (int i = 1; i < threads; i++)
            handles[i] = Thread.Start(&Work);
        Work(0);   // the main thread works too
        for (int i = 1; i < threads; i++)
            Thread.Join(handles[i]);
        return Time.UptimeMs() - start;
    }

    static void Work(ulong unused)
    {
        while (true) {
            long from = Atomic.Add(_next, Chunk) - Chunk;
            if (from >= _limit)
                return;
            long to = from + Chunk < _limit ? from + Chunk : _limit;
            long found = 0;
            for (long n = from; n < to; n++)
                if (IsPrime(n))
                    found++;
            Atomic.Add(_found, found);
        }
    }

    static bool IsPrime(long n)
    {
        if (n < 2) return false;
        if (n % 2 == 0) return n == 2;
        for (long d = 3; d * d <= n; d += 2)
            if (n % d == 0)
                return false;
        return true;
    }

    static long Parse(byte* s, long fallback)
    {
        long v = 0;
        if (*s == 0) return fallback;
        while (*s != 0)
        {
            if (*s < '0' || *s > '9' || v > 1000000000) return fallback;
            v = v * 10 + (*s - '0');
            s++;
        }
        return v;
    }
}
