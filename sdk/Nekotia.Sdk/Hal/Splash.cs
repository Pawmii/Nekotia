namespace Nekotia.Hal;

/// The boot splash (runtime/include/neko/splash.hpp): a progress bar over the night sky while the kernel starts.
/// Everything here does nothing when there is no splash (`nosplash`, tests, host mode).
public static unsafe class Splash
{
    /// What is being started now, and how far the boot is (0–100).
    [Native("::neko::splash::step")] public static extern void Step(string what, uint percent);

    /// The splash fades out and the console, with the whole boot log, comes back.
    [Native("::neko::splash::finish")] public static extern void Finish();
}

/// Sound (runtime/include/neko/audio.hpp): the HDA output with a mixer and a synthesizer, or the PC speaker when
/// there is no sound device. `nosound` on the command line mutes the speaker.
[Native("::neko::audio::Stats")]
public struct SoundStats
{
    public ulong Frames;
    public uint Underruns;
    public uint Clients;
}

public static unsafe class Sound
{
    /// A tone of `hz` for `ms` milliseconds (it sleeps meanwhile).
    [Native("::neko::audio::beep")] public static extern void Beep(uint hz, uint ms);

    /// The stop screen's tune (about three seconds).
    [Native("::neko::audio::play_stop_tune")] public static extern void PlayStopTune();

    /// A sound device is there (/dev/audio exists).
    [Native("::neko::audio::available")] public static extern bool Available();

    [Native("::neko::audio::device_name")] public static extern byte* DeviceName();

    /// Master volume, 0–100.
    [Native("::neko::audio::volume")] public static extern uint Volume();
    [Native("::neko::audio::set_volume")] public static extern void SetVolume(uint percent);

    [Native("::neko::audio::stats")] public static extern SoundStats Stats();
}
