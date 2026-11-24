namespace Nekotia.User;

/// <summary>
/// Sound output (/dev/audio, runtime/include/neko/audio.hpp): 48 kHz, 16-bit signed little-endian, stereo.
/// Writes queue PCM (they block while the 64 KiB queue is full); up to four programs are mixed together.
/// </summary>
public static unsafe class Audio
{
    public const uint Rate = 48000;
    public const uint FrameBytes = 4;

    const ulong GetVolumeRequest = 0x4101, SetVolumeRequest = 0x4102, DrainRequest = 0x4104, QueuedRequest = 0x4105;

    /// <summary>An fd to write PCM to, or a negative error (no sound device: ErrNotFound).</summary>
    public static long Open() => File.Open("/dev/audio", File.Write);

    /// <summary>Master volume 0–100 (for everyone).</summary>
    public static long Volume(int fd) => Device.Control(fd, GetVolumeRequest, null);

    public static long SetVolume(int fd, uint percent) => Device.Control(fd, SetVolumeRequest, (void*)(ulong)percent);

    /// <summary>Waits until everything this program queued has been played.</summary>
    public static long Drain(int fd) => Device.Control(fd, DrainRequest, null);

    /// <summary>Bytes queued and not mixed yet.</summary>
    public static long Queued(int fd) => Device.Control(fd, QueuedRequest, null);
}
