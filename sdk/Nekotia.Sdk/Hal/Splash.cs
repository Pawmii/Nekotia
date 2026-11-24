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

/// The PC speaker (runtime/include/neko/speaker.hpp): one square wave. `nosound` on the command line mutes it.
public static unsafe class Speaker
{
    /// A tone of `hz` for `ms` milliseconds (it sleeps meanwhile).
    [Native("::neko::speaker::beep")] public static extern void Beep(uint hz, uint ms);

    /// The stop screen's tune (it busy-waits about three seconds).
    [Native("::neko::speaker::play_stop_tune")] public static extern void PlayStopTune();
}
