namespace Nekotia.User;

/// <summary>Mirrors <c>neko::user::PollFd</c>: one fd to watch.</summary>
[Native("::neko::user::PollFd")]
public struct PollFd
{
    /// <summary>Negative: the entry is skipped.</summary>
    [Native("fd")] public int Fd;
    /// <summary><see cref="Poll.In"/> | <see cref="Poll.Out"/>.</summary>
    [Native("events")] public ushort Events;
    /// <summary>What is ready (plus <see cref="Poll.Hup"/> / <see cref="Poll.Bad"/>), filled in by Wait.</summary>
    [Native("revents")] public ushort Ready;
}

/// <summary>Waiting for several fds at once: pipes, channels, listeners, /dev/input (runtime/include/neko/vfs.hpp).</summary>
public static unsafe class Poll
{
    public const ushort In = 1, Out = 2, Hup = 4, Bad = 0x8000;

    /// <summary>How many of <paramref name="count"/> entries are ready; 0 after <paramref name="timeoutMs"/> (-1 = for ever).</summary>
    [Native("::neko::user::poll")]
    public static extern long Wait(PollFd* fds, uint count, long timeoutMs);
}

/// <summary>
/// Devices that are more than a byte stream: <c>Map</c> puts an object (shared memory, a framebuffer) into the
/// program's memory, <c>Control</c> is ioctl.
/// </summary>
public static unsafe class Device
{
    public const uint MapWrite = 1;

    /// <summary>The address of the mapping, or a negative error. All of it; <c>Unmap</c> takes it away again.</summary>
    [Native("::neko::user::mmap")]
    public static extern long Map(int fd, ulong length, uint flags);

    [Native("::neko::user::munmap")]
    public static extern long Unmap(void* address);

    [Native("::neko::user::ioctl")]
    public static extern long Control(int fd, ulong request, void* arg);
}

/// <summary>Anonymous shared memory: pages that every process holding the fd can map (runtime/include/neko/ipc.hpp).</summary>
public static unsafe class SharedMemory
{
    /// <summary>A new zeroed object of at least <paramref name="bytes"/> (whole pages, ≤ 64 MiB): its fd or an error.</summary>
    [Native("::neko::user::shm_create")]
    public static extern long Create(ulong bytes);
}

/// <summary>
/// Message channels between programs: a server listens under a name, clients connect, and each message
/// (≤ 4 KiB) may carry an open file — how a window's pixels (shared memory) reach the compositor.
/// </summary>
public static unsafe class Channel
{
    public const int MaxMessage = 4096;

    /// <summary>An fd that accepts connections to <paramref name="name"/> (ErrExists when it is taken).</summary>
    [Native("::neko::user::ipc_listen")]
    public static extern long Listen(string name);

    [Native("::neko::user::ipc_listen")]
    public static extern long Listen(byte* name);

    /// <summary>A channel to the program listening under <paramref name="name"/> (ErrNotFound when nobody is).</summary>
    [Native("::neko::user::ipc_connect")]
    public static extern long Connect(string name);

    [Native("::neko::user::ipc_connect")]
    public static extern long Connect(byte* name);

    /// <summary>The server end of the next connection (waits for one; poll the listener to avoid waiting).</summary>
    [Native("::neko::user::ipc_accept")]
    public static extern long Accept(int listenFd);

    /// <summary>One message; <paramref name="attachFd"/> (-1 = none) travels with it. Never waits: ErrBusy when the
    /// peer's queue is full, ErrBrokenPipe when the peer is gone.</summary>
    [Native("::neko::user::ipc_send")]
    public static extern long Send(int fd, void* data, ulong length, int attachFd);

    /// <summary>Waits for a message: its length, 0 once the peer is gone. <paramref name="fdOut"/> gets the fd of a file
    /// that came with it, or -1.</summary>
    [Native("::neko::user::ipc_recv")]
    public static extern long Receive(int fd, void* buffer, ulong capacity, int* fdOut);
}
