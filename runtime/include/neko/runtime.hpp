// Everything generated code may reference. Included by build/gen/nekotia.gen.hpp.
#pragma once

#include <neko/types.hpp>
#include <neko/block.hpp>
#include <neko/net.hpp>
#include <neko/usb.hpp>
#include <neko/boot.hpp>
#include <neko/console.hpp>
#include <neko/display.hpp>
#include <neko/input.hpp>
#include <neko/ipc.hpp>
#include <neko/crash.hpp>
#include <neko/audio.hpp>
#include <neko/speaker.hpp>
#include <neko/splash.hpp>
#include <neko/exception.hpp>
#include <neko/fat.hpp>
#include <neko/ext2.hpp>
#include <neko/fmt.hpp>
#include <neko/log.hpp>
#include <neko/cpu.hpp>
#include <neko/math.hpp>
#include <neko/memory.hpp>
#include <neko/serial.hpp>
#include <neko/platform.hpp>
#include <neko/pci.hpp>
#include <neko/process.hpp>
#include <neko/sched.hpp>
#include <neko/smp.hpp>
#include <neko/vfs.hpp>

namespace neko {

// Shows the stop screen (KERNEL_PANIC) and stops. See neko/crash.hpp for the other stop codes.
NEKO_NORETURN void panic(const char* message);

namespace platform {
NEKO_NORETURN void stop_after_panic();  // implemented per platform
}

}  // namespace neko
