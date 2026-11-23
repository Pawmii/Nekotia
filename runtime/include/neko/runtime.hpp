// Everything generated code may reference. Included by build/gen/nekotia.gen.hpp.
#pragma once

#include <neko/types.hpp>
#include <neko/boot.hpp>
#include <neko/cpu.hpp>
#include <neko/memory.hpp>
#include <neko/serial.hpp>

namespace neko {

// Prints "PANIC: <message>" and stops (hardware: cli; hlt forever, host: exit(1)).
NEKO_NORETURN void panic(const char* message);

namespace platform {
NEKO_NORETURN void stop_after_panic();  // implemented per platform
}

}  // namespace neko
