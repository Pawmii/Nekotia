// fetch [-i] URL — HTTP/1.0 GET; the body goes to stdout (`fetch URL > /mnt/file` downloads),
// -i prints the response headers too. URL: [http://]host[:port][/path]
using Nekotia;
using Nekotia.User;

namespace FetchProgram;

public static unsafe class Program
{
    public static int Main(int argc, byte** argv)
    {
        bool headers = argc > 2 && Str.Equals(argv[1], "-i");
        if (argc != (headers ? 3 : 2))
        {
            Console.WriteLine("usage: fetch [-i] [http://]HOST[:PORT][/PATH]   (e.g. fetch 10.0.2.2:8000/)");
            return 2;
        }
        byte* url = argv[headers ? 2 : 1];
        if (Str.StartsWith(url, "http://"))
            url += 7;
        // host[:port][/path]
        byte* host = stackalloc byte[128];
        int n = 0;
        while (*url != 0 && *url != ':' && *url != '/' && n < 127)
            host[n++] = *url++;
        host[n] = 0;
        ushort port = 80;
        if (*url == ':')
        {
            url++;
            port = 0;
            while (*url >= '0' && *url <= '9')
                port = (ushort)(port * 10 + (*url++ - '0'));
        }
        byte* path = *url == '/' ? url : Str.Bytes("/");

        uint ip = 0;
        long e = Net.Resolve(host, out ip);
        if (e < 0)
            return Tool.Fail("fetch", host, e);
        long fd = Net.Connect(ip, port);
        if (fd < 0)
            return Tool.Fail("fetch", host, fd);

        Net.WriteAll((int)fd, "GET ");
        Net.WriteAll((int)fd, path, (long)Str.Length(path));
        Net.WriteAll((int)fd, " HTTP/1.0\r\nHost: ");
        Net.WriteAll((int)fd, host, (long)Str.Length(host));
        Net.WriteAll((int)fd, "\r\nUser-Agent: nekotia-fetch\r\nConnection: close\r\n\r\n");

        byte* buffer = (byte*)Memory.Alloc(8192);
        int matched = 0;          // how much of "\r\n\r\n" we have seen
        bool inBody = headers;
        long body = 0;
        while (true)
        {
            long got = File.ReadBytes((int)fd, buffer, 8192);
            if (got <= 0)
                break;
            long start = 0;
            if (!inBody)
            {
                while (start < got && matched < 4)
                {
                    byte c = buffer[start++];
                    matched = c == (matched % 2 == 0 ? '\r' : '\n') ? matched + 1 : (c == '\r' ? 1 : 0);
                }
                if (matched < 4)
                    continue;
                inBody = true;
            }
            body += got - start;
            if (!Net.WriteAll(1, buffer + start, got - start))
                break;
        }
        File.Close((int)fd);
        return 0;
    }
}
