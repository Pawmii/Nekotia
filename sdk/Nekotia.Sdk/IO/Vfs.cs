namespace Nekotia.IO;

/// <summary>Mirrors <c>neko::vfs::Type</c>.</summary>
[Native("::neko::vfs::Type")]
public enum NodeType : byte { None = 0, File = 1, Directory = 2, Device = 3 }

/// <summary>Mirrors <c>neko::vfs::Stat</c>.</summary>
[Native("::neko::vfs::Stat")]
public struct FileStat
{
    [Native("type")] public NodeType Type;
    /// <summary>Node number, unique while the node exists.</summary>
    [Native("id")] public uint Id;
    /// <summary>Bytes for files, entries for directories.</summary>
    [Native("size")] public ulong Size;
    [Native("created_ms")] public ulong CreatedMs;
    [Native("modified_ms")] public ulong ModifiedMs;
}

/// <summary>One entry of a directory. Mirrors <c>neko::vfs::DirEntry</c>.</summary>
[Native("::neko::vfs::DirEntry")]
public unsafe struct DirEntry
{
    [Native("name")] public fixed byte Name[64];
    [Native("type")] public NodeType Type;
    [Native("size")] public ulong Size;
}

/// <summary>An open file of the kernel (opaque; <see cref="Vfs.Open(string, uint, out OpenFile*)"/>).</summary>
[Native("::neko::vfs::File")]
public struct OpenFile { }

/// <summary>Mirrors <c>neko::vfs::Stats</c>.</summary>
[Native("::neko::vfs::Stats")]
public struct VfsStats
{
    [Native("nodes")] public ulong Nodes;
    [Native("open_files")] public ulong OpenFiles;
    /// <summary>File contents copied into the kernel heap.</summary>
    [Native("heap_bytes")] public ulong HeapBytes;
    /// <summary>File contents still read straight from the initrd module.</summary>
    [Native("initrd_bytes")] public ulong InitrdBytes;
    [Native("initrd_entries")] public ulong InitrdEntries;
}

/// <summary>
/// The virtual file system (runtime/src/core/vfs.cpp): ramfs at /, the initrd unpacked into it, devices in /dev.
/// Paths are absolute or relative to the working directory (kernel threads share one). Results are 0 / counts,
/// or negative errors (<see cref="Nekotia.Processes.Process.ErrorText"/> names them).
/// </summary>
public static unsafe class Vfs
{
    /// <summary>Open flags.</summary>
    public const uint Read = 1, Write = 2, Create = 4, Truncate = 8, Append = 16, Exclusive = 32;

    /// <summary>Seek origins.</summary>
    public const int SeekSet = 0, SeekCurrent = 1, SeekEnd = 2;

    public const int MaxName = 63, MaxPath = 256;

    [Native("::neko::vfs::open")]
    public static extern long Open(string path, uint flags, out OpenFile* file);

    [Native("::neko::vfs::open")]
    public static extern long Open(byte* path, uint flags, out OpenFile* file);

    /// <summary>Bytes read, 0 at the end of the file.</summary>
    [Native("::neko::vfs::read")]
    public static extern long ReadFile(OpenFile* file, void* buffer, ulong length);

    [Native("::neko::vfs::write")]
    public static extern long WriteFile(OpenFile* file, void* buffer, ulong length);

    [Native("::neko::vfs::seek")]
    public static extern long Seek(OpenFile* file, long offset, int whence);

    [Native("::neko::vfs::truncate")]
    public static extern long SetLength(OpenFile* file, ulong size);

    [Native("::neko::vfs::fstat")]
    public static extern long Stat(OpenFile* file, out FileStat stat);

    /// <summary>Entry <paramref name="index"/> of an open directory: 1, 0 past the end, or an error.</summary>
    [Native("::neko::vfs::readdir")]
    public static extern long ReadDirectory(OpenFile* file, uint index, out DirEntry entry);

    [Native("::neko::vfs::close")]
    public static extern void Close(OpenFile* file);

    [Native("::neko::vfs::stat")]
    public static extern long Stat(string path, out FileStat stat);

    [Native("::neko::vfs::stat")]
    public static extern long Stat(byte* path, out FileStat stat);

    [Native("::neko::vfs::mkdir")]
    public static extern long CreateDirectory(string path);

    [Native("::neko::vfs::mkdir")]
    public static extern long CreateDirectory(byte* path);

    /// <summary>Removes a file or an empty directory (open files keep working until closed).</summary>
    [Native("::neko::vfs::remove")]
    public static extern long Remove(string path);

    [Native("::neko::vfs::remove")]
    public static extern long Remove(byte* path);

    [Native("::neko::vfs::rename")]
    public static extern long Rename(string from, string to);

    [Native("::neko::vfs::rename")]
    public static extern long Rename(byte* from, byte* to);

    [Native("::neko::vfs::chdir")]
    public static extern long ChangeDirectory(string path);

    [Native("::neko::vfs::chdir")]
    public static extern long ChangeDirectory(byte* path);

    /// <summary>Writes the working directory into <paramref name="buffer"/>; its length or an error.</summary>
    [Native("::neko::vfs::getcwd")]
    public static extern long CurrentDirectory(byte* buffer, ulong capacity);

    [Native("::neko::vfs::unpack_tar")]
    public static extern long UnpackTar(string directory, void* data, ulong size);

    /// <summary>What reads of /dev/console call (the keyboard line discipline).</summary>
    [Native("::neko::vfs::set_console_reader")]
    public static extern void SetConsoleReader(delegate*<byte*, ulong, long> reader);

    [Native("::neko::vfs::stats")]
    public static extern VfsStats Statistics();

    public static bool Exists(string path)
    {
        FileStat st = default;
        return Stat(path, out st) == 0;
    }

    public static bool Exists(byte* path)
    {
        FileStat st = default;
        return Stat(path, out st) == 0;
    }

    /// <summary>Creates or replaces a file with <paramref name="length"/> bytes.</summary>
    public static long WriteAll(string path, void* data, ulong length)
    {
        OpenFile* f = null;
        long e = Open(path, Write | Create | Truncate, out f);
        if (e < 0)
            return e;
        long n = WriteFile(f, data, length);
        Close(f);
        return n;
    }
}
