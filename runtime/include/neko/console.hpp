// Text output.
//   neko::console — framebuffer text console (PSF font, UTF-8, a subset of ANSI escapes)
//   neko::term    — the kernel's main output: console + serial at the same time
#pragma once

#include <neko/boot.hpp>
#include <neko/types.hpp>

namespace neko::console {

// Needs the heap (cell buffer). Returns false without a usable 32-bpp framebuffer.
bool init(const Framebuffer& fb);
bool ready();

void put_byte(uint8_t b);          // UTF-8 bytes, \n \r \b \t, ESC[...m / ESC[2J / ESC[H / ESC[K
void put_codepoint(uint32_t cp);

void clear();
void set_colors(uint32_t fg, uint32_t bg);  // 0xRRGGBB
void reset_colors();
uint32_t columns();
uint32_t rows();
uint32_t cursor_column();
uint32_t cursor_row();
void set_cursor(uint32_t column, uint32_t row);
void set_cursor_visible(bool visible);
void redraw();  // repaint everything (after something else drew on the framebuffer)
// While paused (a display server owns the screen, neko/display.hpp) the console keeps its text but
// draws nothing; un-pausing repaints the whole screen. The stop screen un-pauses it first.
void set_paused(bool paused);
bool paused();
// Bumped on every pixel the console draws (displays that need PRESENT flush when it moves).
uint64_t changes();
// Another framebuffer of the same size takes over (a display driver replaced the bootloader's): repaint there.
void retarget(const Framebuffer& fb);

// Draws ASCII/UTF-8 text `scale` times larger, top-left at cell (column, row), straight to the framebuffer.
// The cells underneath are not updated — meant for screens that are drawn once (the stop screen).
void draw_large(uint32_t column, uint32_t row, const char* text, uint32_t fg, uint32_t bg, uint32_t scale);

}  // namespace neko::console

namespace neko::term {

void write_char(uint8_t c);
void write(const char* s);
void write(const uint8_t* s);
void write_codepoint(uint32_t cp);  // encodes as UTF-8
void write_hex(uint64_t value);
void write_dec(int64_t value);
void write_udec(uint64_t value);
void write_size(uint64_t bytes);
void write_float(double value, int32_t decimals);  // fixed notation, e.g. 3.142
void pause_console(bool paused);  // console::set_paused under the output lock

}  // namespace neko::term
