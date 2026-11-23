namespace Nekotia.User;

/// <summary>What a path names. Values of <c>neko::vfs::Type</c>.</summary>
public enum NodeType : byte { None = 0, File = 1, Directory = 2, Device = 3 }

/// <summary>Mirrors <c>neko::user::Stat</c> (the kernel's <c>neko::vfs::Stat</c>).</summary>
[Native("::neko::user::Stat")]
public struct FileStat
{
    [Native("type")] public byte Type;
    /// <summary>Node number, unique while the node exists.</summary>
    [Native("id")] public uint Id;
    /// <summary>Bytes for files, entries for directories.</summary>
    [Native("size")] public ulong Size;
    [Native("created_ms")] public ulong CreatedMs;
    [Native("modified_ms")] public ulong ModifiedMs;
}

/// <summary>One directory entry. Mirrors <c>neko::user::DirEntry</c>.</summary>
[Native("::neko::user::DirEntry")]
public unsafe struct DirEntry
{
    [Native("name")] public fixed byte Name[64];
    [Native("type")] public byte Type;
    [Native("size")] public ulong Size;
}

/// <summary>
/// Files and directories (system calls 16–29, runtime/src/core/vfs.cpp). File descriptors are ints: 0 stdin,
/// 1 stdout, 2 stderr — inherited from the parent, so a shell can redirect them with <see cref="Redirect"/>.
/// Paths are absolute or relative to <see cref="CurrentDirectory"/>. Negative results are errors
/// (<see cref="Process.ErrorText"/>).
/// </summary>
public static unsafe class File
{
    /// <summary>Open flags.</summary>
    public const uint Read = 1, Write = 2, Create = 4, Truncate = 8, Append = 16, Exclusive = 32;

    /// <summary>Seek origins.</summary>
    public const int SeekSet = 0, SeekCurrent = 1, SeekEnd = 2;

    public const int MaxPath = 256;

    /// <summary>Opens <paramref name="path"/>; the file descriptor or a negative error.</summary>
    [Native("::neko::user::open")]
    public static extern long Open(byte* path, uint flags);

    [Native("::neko::user::open")]
    public static extern long Open(string path, uint flags);

    [Native("::neko::user::close")]
    public static extern long Close(int fd);

    /// <summary>Bytes read, 0 at the end. From the console: one line once Enter is pressed.</summary>
    [Native("::neko::user::read")]
    public static extern long ReadBytes(int fd, void* buffer, ulong length);

    [Native("::neko::user::write")]
    public static extern long WriteBytes(int fd, void* data, ulong length);

    /// <summary>New offset, from <see cref="SeekSet"/>, <see cref="SeekCurrent"/> or <see cref="SeekEnd"/>.</summary>
    [Native("::neko::user::seek")]
    public static extern long Seek(int fd, long offset, int whence);

    [Native("::neko::user::stat")]
    public static extern long Stat(byte* path, out FileStat stat);

    [Native("::neko::user::stat")]
    public static extern long Stat(string path, out FileStat stat);

    [Native("::neko::user::fstat")]
    public static extern long Stat(int fd, out FileStat stat);

    /// <summary>Entry <paramref name="index"/> of directory <paramref name="fd"/>: 1, 0 past the end, or an error.</summary>
    [Native("::neko::user::readdir")]
    public static extern long ReadDirectory(int fd, uint index, out DirEntry entry);

    [Native("::neko::user::mkdir")]
    public static extern long CreateDirectory(byte* path);

    [Native("::neko::user::mkdir")]
    public static extern long CreateDirectory(string path);

    /// <summary>Removes a file or an empty directory.</summary>
    [Native("::neko::user::remove")]
    public static extern long Delete(byte* path);

    [Native("::neko::user::remove")]
    public static extern long Delete(string path);

    [Native("::neko::user::rename")]
    public static extern long Rename(byte* from, byte* to);

    [Native("::neko::user::chdir")]
    public static extern long ChangeDirectory(byte* path);

    [Native("::neko::user::chdir")]
    public static extern long ChangeDirectory(string path);

    /// <summary>Writes the working directory into <paramref name="buffer"/>; its length or an error.</summary>
    [Native("::neko::user::getcwd")]
    public static extern long CurrentDirectory(byte* buffer, ulong capacity);

    /// <summary>A second descriptor for the same open file (shared offset).</summary>
    [Native("::neko::user::dup")]
    public static extern long Duplicate(int fd);

    /// <summary>Makes <paramref name="target"/> refer to the file of <paramref name="fd"/> (closing what it was).</summary>
    [Native("::neko::user::dup2")]
    public static extern long Redirect(int fd, int target);

    [Native("::neko::user::truncate")]
    public static extern long SetLength(int fd, ulong size);

    public static bool Exists(byte* path)
    {
        FileStat st = default;
        return Stat(path, out st) == 0;
    }

    public static bool IsDirectory(byte* path)
    {
        FileStat st = default;
        return Stat(path, out st) == 0 && st.Type == (byte)NodeType.Directory;
    }
}
