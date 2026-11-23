// httpd [PORT] [DIR] — a small HTTP/1.0 file server (default: port 80, directory /). Files are sent with
// a content type from their extension, directories as an HTML listing. One thread per connection.
// Under `make run`, the host's http://localhost:8080/ reaches port 80 here.
using Nekotia;
using Nekotia.User;

namespace HttpdProgram;

public static unsafe class Program
{
    const int RequestSize = 2048;
    static byte* _root;
    static long _served;

    public static int Main(int argc, byte** argv)
    {
        ushort port = 80;
        if (argc > 1)
        {
            port = 0;
            for (byte* p = argv[1]; *p >= '0' && *p <= '9'; p++)
                port = (ushort)(port * 10 + (*p - '0'));
        }
        _root = argc > 2 ? argv[2] : Str.Bytes("/");
        if (!File.IsDirectory(_root))
            return Tool.Fail("httpd", _root, Process.ErrNotDirectory);
        long listener = Net.Listen(port);
        if (listener < 0)
            return Tool.Fail("httpd", null, listener);
        uint* config = stackalloc uint[4];
        Net.Config(config);
        Console.Write(Ansi.Green, "httpd: serving ");
        Console.Write(_root);
        Console.Write(" on http://");
        Net.WriteIp(config[0]);
        Console.WriteLine($":{port}/ — Ctrl+C stops");
        while (true)
        {
            uint* peer = (uint*)Memory.Alloc(16);
            long fd = Net.Accept((int)listener, peer);
            if (fd < 0)
            {
                Memory.Free(peer);
                return Tool.Fail("httpd", null, fd);
            }
            peer[2] = (uint)fd;
            if (Thread.Start(&Serve, (ulong)peer) == 0)
            {
                File.Close((int)fd);
                Memory.Free(peer);
            }
        }
    }

    static void Serve(ulong arg)
    {
        uint* peer = (uint*)arg;
        int fd = (int)peer[2];
        byte* request = (byte*)Memory.Alloc(RequestSize);
        byte* path = (byte*)Memory.Alloc(File.MaxPath);
        int length = 0;
        // Read until the blank line that ends the request headers.
        while (length < RequestSize - 1)
        {
            long n = File.ReadBytes(fd, request + length, (ulong)(RequestSize - 1 - length));
            if (n <= 0)
                break;
            length += (int)n;
            request[length] = 0;
            if (Contains(request, "\r\n\r\n") || Contains(request, "\n\n"))
                break;
        }
        request[length] = 0;
        int status = Handle(fd, request, path);
        Console.Write(Ansi.Gray);
        Net.WriteIp(peer[0]);
        Console.Write(" ");
        int i = 0;
        while (request[i] != 0 && request[i] != '\r' && request[i] != '\n' && i < 80)
            Console.WriteChar(request[i++]);
        Console.WriteLine($" → {status}{Ansi.Reset}");
        File.Close(fd);
        Memory.Free(request);
        Memory.Free(path);
        Memory.Free(peer);
    }

    static bool Contains(byte* s, string what)
    {
        byte* w = Str.Bytes(what);
        for (; *s != 0; s++)
        {
            int k = 0;
            while (w[k] != 0 && s[k] == w[k])
                k++;
            if (w[k] == 0)
                return true;
        }
        return false;
    }

    static int Handle(int fd, byte* request, byte* path)
    {
        if (!Str.StartsWith(request, "GET ") && !Str.StartsWith(request, "HEAD "))
            return Error(fd, 405, "Method Not Allowed");
        bool head = request[0] == 'H';
        byte* url = request + (head ? 5 : 4);
        // root + decoded URL path (without the query string)
        int n = 0;
        for (byte* r = _root; *r != 0 && n < File.MaxPath - 2; r++)
            path[n++] = *r;
        if (n > 0 && path[n - 1] == '/')
            n--;
        while (*url != 0 && *url != ' ' && *url != '?' && n < File.MaxPath - 1)
        {
            byte c = *url++;
            if (c == '%' && Hex(url[0]) >= 0 && Hex(url[1]) >= 0)
            {
                c = (byte)(Hex(url[0]) * 16 + Hex(url[1]));
                url += 2;
            }
            path[n++] = c;
        }
        path[n] = 0;
        if (n == 0)
        {
            path[0] = (byte)'/';
            path[1] = 0;
        }
        if (Contains(path, ".."))
            return Error(fd, 403, "Forbidden");

        FileStat st = default;
        if (File.Stat(path, out st) < 0)
            return Error(fd, 404, "Not Found");
        if (st.Type == (byte)NodeType.Directory)
            return Listing(fd, path, request, head);
        long file = File.Open(path, File.Read);
        if (file < 0)
            return Error(fd, 403, "Forbidden");
        Header(fd, 200, "OK", Type(path), (long)st.Size);
        if (!head)
        {
            byte* buffer = (byte*)Memory.Alloc(16384);
            while (true)
            {
                long got = File.ReadBytes((int)file, buffer, 16384);
                if (got <= 0 || !Net.WriteAll(fd, buffer, got))
                    break;
            }
            Memory.Free(buffer);
        }
        File.Close((int)file);
        return 200;
    }

    static int Hex(byte c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static string Type(byte* path)
    {
        byte* dot = null;
        for (byte* p = path; *p != 0; p++)
            if (*p == '.') dot = p;
        if (dot == null) return "application/octet-stream";
        if (Str.Equals(dot, ".html") || Str.Equals(dot, ".htm")) return "text/html; charset=utf-8";
        if (Str.Equals(dot, ".txt") || Str.Equals(dot, ".cs") || Str.Equals(dot, ".neko") || Str.Equals(dot, ".md")) return "text/plain; charset=utf-8";
        if (Str.Equals(dot, ".css")) return "text/css";
        if (Str.Equals(dot, ".js")) return "text/javascript";
        if (Str.Equals(dot, ".png")) return "image/png";
        if (Str.Equals(dot, ".jpg") || Str.Equals(dot, ".jpeg")) return "image/jpeg";
        if (Str.Equals(dot, ".svg")) return "image/svg+xml";
        return "application/octet-stream";
    }

    static void Header(int fd, int status, string text, string type, long length)
    {
        Net.WriteAll(fd, "HTTP/1.0 ");
        Number(fd, status);
        Net.WriteAll(fd, " ");
        Net.WriteAll(fd, text);
        Net.WriteAll(fd, "\r\nServer: Nekotia httpd (C# in ring 3)\r\nContent-Type: ");
        Net.WriteAll(fd, type);
        if (length >= 0)
        {
            Net.WriteAll(fd, "\r\nContent-Length: ");
            Number(fd, length);
        }
        Net.WriteAll(fd, "\r\nConnection: close\r\n\r\n");
    }

    static void Number(int fd, long v)
    {
        byte* digits = stackalloc byte[24];
        int n = 0;
        do
        {
            digits[n++] = (byte)('0' + v % 10);
            v /= 10;
        } while (v > 0);
        for (int i = n - 1; i >= 0; i--)
            Net.WriteAll(fd, digits + i, 1);
    }

    static int Error(int fd, int status, string text)
    {
        Header(fd, status, text, "text/html; charset=utf-8", -1);
        Net.WriteAll(fd, "<!doctype html><title>");
        Net.WriteAll(fd, text);
        Net.WriteAll(fd, "</title><h1>");
        Number(fd, status);
        Net.WriteAll(fd, " ");
        Net.WriteAll(fd, text);
        Net.WriteAll(fd, "</h1><p>Nekotia httpd</p>\n");
        return status;
    }

    static int Listing(int fd, byte* path, byte* request, bool head)
    {
        long dir = File.Open(path, File.Read);
        if (dir < 0)
            return Error(fd, 403, "Forbidden");
        Header(fd, 200, "OK", "text/html; charset=utf-8", -1);
        if (head)
        {
            File.Close((int)dir);
            return 200;
        }
        // The URL path as the browser sent it (for links and the title).
        byte* url = request + 4;
        int urlLength = 0;
        while (url[urlLength] != 0 && url[urlLength] != ' ' && url[urlLength] != '?')
            urlLength++;
        bool slash = urlLength > 0 && url[urlLength - 1] == '/';
        Net.WriteAll(fd, "<!doctype html><meta charset=utf-8><title>Nekotia: ");
        Net.WriteAll(fd, url, urlLength);
        Net.WriteAll(fd, "</title><style>body{font:15px/1.5 system-ui,sans-serif;margin:2em auto;max-width:44em;padding:0 1em;color:#222;background:#fdfcfb}a{color:#7a3db8;text-decoration:none}a:hover{text-decoration:underline}td{padding:.15em 1.2em .15em 0}.s{color:#777;text-align:right}h1{font-weight:600}</style>");
        Net.WriteAll(fd, "<h1>🐾 ");
        Net.WriteAll(fd, url, urlLength);
        Net.WriteAll(fd, "</h1><table>");
        if (urlLength > 1)
            Net.WriteAll(fd, "<tr><td><a href=\"..\">..</a></td><td></td></tr>");
        DirEntry e = default;
        for (uint i = 0; File.ReadDirectory((int)dir, i, out e) == 1; i++)
        {
            Net.WriteAll(fd, "<tr><td><a href=\"");
            if (!slash)
            {
                // relative links need the directory itself: /mnt → /mnt/name
                Net.WriteAll(fd, url, urlLength);
                Net.WriteAll(fd, "/");
            }
            Net.WriteAll(fd, e.Name, (long)Str.Length(e.Name));
            if (e.Type == (byte)NodeType.Directory)
                Net.WriteAll(fd, "/");
            Net.WriteAll(fd, "\">");
            Net.WriteAll(fd, e.Name, (long)Str.Length(e.Name));
            if (e.Type == (byte)NodeType.Directory)
                Net.WriteAll(fd, "/");
            Net.WriteAll(fd, "</a></td><td class=s>");
            if (e.Type == (byte)NodeType.File)
            {
                Number(fd, (long)e.Size);
                Net.WriteAll(fd, " B");
            }
            else if (e.Type == (byte)NodeType.Device)
                Net.WriteAll(fd, "device");
            Net.WriteAll(fd, "</td></tr>");
        }
        File.Close((int)dir);
        Net.WriteAll(fd, "</table><p style=\"color:#999\">served by a C# program in ring 3 of Nekotia</p>\n");
        return 200;
    }
}
