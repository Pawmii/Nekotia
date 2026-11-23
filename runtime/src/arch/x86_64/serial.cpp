// 16550 UART on COM1, 115200 baud 8N1, polled.
#include <neko/runtime.hpp>

namespace neko::serial {
namespace {

constexpr uint16_t kCom1 = 0x3F8;
bool g_present = false;

}  // namespace

void init() {
    using namespace neko::cpu;
    outb(kCom1 + 1, 0x00);  // no interrupts
    outb(kCom1 + 3, 0x80);  // DLAB on
    outb(kCom1 + 0, 0x01);  // divisor 1 → 115200 baud
    outb(kCom1 + 1, 0x00);
    outb(kCom1 + 3, 0x03);  // 8 bits, no parity, one stop bit
    outb(kCom1 + 2, 0xC7);  // FIFO on, cleared, 14-byte threshold
    outb(kCom1 + 4, 0x1E);  // loopback mode for the self test
    outb(kCom1 + 0, 0xAE);
    if (inb(kCom1 + 0) != 0xAE) return;  // no UART
    outb(kCom1 + 4, 0x0F);  // normal operation
    g_present = true;
}

void write_char(uint8_t c) {
    if (!g_present) return;
    if (c == '\n') write_char('\r');
    while ((cpu::inb(kCom1 + 5) & 0x20) == 0) {
    }
    cpu::outb(kCom1, c);
}

}  // namespace neko::serial
