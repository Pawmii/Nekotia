// Time, interrupts, power and system information — the services the C# kernel builds on.
#pragma once

#include <neko/types.hpp>

namespace neko::time {

uint64_t uptime_ms();        // milliseconds since the timer started (LAPIC timer, 1 kHz)
void sleep_ms(uint64_t ms);  // halts until the time passes; needs interrupts enabled
uint32_t timer_hz();

}  // namespace neko::time

namespace neko::irq {

using Handler = void (*)();

// Routes legacy ISA IRQ `line` (0–15, e.g. 1 = PS/2 keyboard) through the IOAPIC to `handler`.
// The handler runs with interrupts disabled; EOI is sent by the runtime. It must not use the heap.
bool register_handler(uint8_t line, Handler handler);
uint64_t count(uint8_t line);  // how many times the line fired

// Message-signalled interrupts (PCI MSI / MSI-X): a free vector whose handler gets `context`.
// The device is programmed with msi_address() / msi_data(vector). Returns the vector, 0 when none is left.
using MsiHandler = void (*)(void* context);
uint8_t allocate_msi(MsiHandler handler, void* context, const char* name);
uint64_t msi_address();          // the boot CPU's local APIC
uint32_t msi_data(uint8_t vector);  // edge-triggered, fixed delivery
// Statistics of the allocated vectors, for `irq`: false past the end.
bool msi_info(uint32_t index, uint8_t& vector, const char*& name, uint64_t& count);

}  // namespace neko::irq

namespace neko::power {

NEKO_NORETURN void reboot();
NEKO_NORETURN void shutdown();  // ACPI-less: works in QEMU/Bochs, otherwise halts
// QEMU isa-debug-exit (port 0xf4): QEMU exits with status (code << 1) | 1. Used by `make test`.
NEKO_NORETURN void test_exit(uint8_t code);

}  // namespace neko::power

namespace neko::sysinfo {

const char* cpu_brand();     // CPUID brand string
uint32_t cpu_count();        // enabled processors in the ACPI MADT
uint64_t cpu_mhz();          // TSC frequency measured at boot (0 if unknown)
bool apic_available();

}  // namespace neko::sysinfo

// Implemented by nekoc-generated code: registers [Irq] handlers. Called before interrupts are enabled.
extern "C" void neko_kernel_init();
