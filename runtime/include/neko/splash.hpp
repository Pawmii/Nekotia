// The boot splash: a night sky with a cat, the name and a progress bar while the kernel starts; the console
// (with the whole boot log) fades in when it ends. Drawn by the CPU into a back buffer in RAM and copied to
// the console's framebuffer (it follows the console to a GPU driver's buffer, console::retarget). Kernel
// command line: `nosplash` shows the log instead; `test` never has a splash.
#pragma once

#include <neko/boot.hpp>
#include <neko/types.hpp>

namespace neko::splash {

void begin(const char* cmdline);  // covers the console (after console::init)
void animate();                   // starts the animation thread: needs the scheduler and interrupts
void step(const char* what, uint32_t percent);
void finish();                    // fades out; the console comes back
void abort();                     // the stop screen: gone at once, no locks taken
bool active();
void retarget(const Framebuffer& fb);  // console::retarget while covered

}  // namespace neko::splash
