// Framebuffer text console.
//
// The screen is a grid of cells (codepoint + colours). Writes update the grid; a
// cell is repainted only when it differs from what is already on screen, so
// scrolling a mostly empty screen stays cheap even on slow framebuffers.
#include <neko/console.hpp>
#include <neko/runtime.hpp>

extern "C" const uint8_t neko_font_psf[];
extern "C" const uint8_t neko_font_psf_end[];

namespace neko::console {
namespace {

// ---------------------------------------------------------------- font

struct Font {
    const uint8_t* glyphs = nullptr;
    uint32_t count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytes_per_row = 0;
    uint32_t bytes_per_glyph = 0;
};

constexpr uint32_t kDirectMapSize = 0x2600;  // Latin, Cyrillic, Greek, punctuation, box drawing, blocks
Font g_font;
uint16_t g_glyph_of[kDirectMapSize];         // glyph index + 1, 0 = not in the font
uint32_t g_fallback_glyph = 0;

uint16_t read16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t read32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

void map_glyph(uint32_t cp, uint32_t glyph) {
    if (cp < kDirectMapSize && g_glyph_of[cp] == 0) g_glyph_of[cp] = uint16_t(glyph + 1);
}

bool load_psf1(const uint8_t* data, size_t size) {
    uint8_t mode = data[2];
    g_font.count = (mode & 0x01) ? 512 : 256;
    g_font.width = 8;
    g_font.height = data[3];
    g_font.bytes_per_row = 1;
    g_font.bytes_per_glyph = g_font.height;
    g_font.glyphs = data + 4;

    const uint8_t* table = g_font.glyphs + g_font.count * g_font.bytes_per_glyph;
    if (!(mode & 0x06)) {  // no Unicode table: assume Latin-1 order
        for (uint32_t i = 0; i < g_font.count; ++i) map_glyph(i, i);
        return true;
    }
    const uint8_t* end = data + size;
    for (uint32_t glyph = 0; glyph < g_font.count && table + 2 <= end; ++glyph) {
        bool in_sequence = false;
        for (; table + 2 <= end; table += 2) {
            uint16_t v = read16(table);
            if (v == 0xFFFF) {
                table += 2;
                break;
            }
            if (v == 0xFFFE) in_sequence = true;
            else if (!in_sequence) map_glyph(v, glyph);
        }
    }
    return true;
}

bool load_psf2(const uint8_t* data, size_t size) {
    uint32_t header_size = read32(data + 8);
    uint32_t flags = read32(data + 12);
    g_font.count = read32(data + 16);
    g_font.bytes_per_glyph = read32(data + 20);
    g_font.height = read32(data + 24);
    g_font.width = read32(data + 28);
    g_font.bytes_per_row = (g_font.width + 7) / 8;
    g_font.glyphs = data + header_size;

    const uint8_t* table = g_font.glyphs + size_t(g_font.count) * g_font.bytes_per_glyph;
    if (!(flags & 1)) {
        for (uint32_t i = 0; i < g_font.count; ++i) map_glyph(i, i);
        return true;
    }
    const uint8_t* end = data + size;
    for (uint32_t glyph = 0; glyph < g_font.count && table < end; ++glyph) {
        bool in_sequence = false;
        while (table < end && *table != 0xFF) {
            if (*table == 0xFE) {
                in_sequence = true;
                ++table;
                continue;
            }
            // decode one UTF-8 codepoint
            uint32_t cp = *table, extra = 0;
            if (cp >= 0xF0) cp &= 0x07, extra = 3;
            else if (cp >= 0xE0) cp &= 0x0F, extra = 2;
            else if (cp >= 0xC0) cp &= 0x1F, extra = 1;
            ++table;
            for (; extra && table < end; --extra) cp = (cp << 6) | (*table++ & 0x3F);
            if (!in_sequence) map_glyph(cp, glyph);
        }
        ++table;  // 0xFF
    }
    return true;
}

bool load_font() {
    const uint8_t* data = neko_font_psf;
    size_t size = size_t(neko_font_psf_end - neko_font_psf);
    bool ok = false;
    if (size >= 4 && data[0] == 0x36 && data[1] == 0x04) ok = load_psf1(data, size);
    else if (size >= 32 && read32(data) == 0x864AB572) ok = load_psf2(data, size);
    if (!ok || g_font.width == 0 || g_font.height == 0) return false;
    g_fallback_glyph = g_glyph_of['?'] ? g_glyph_of['?'] - 1u : 0;
    return true;
}

uint32_t glyph_for(uint32_t cp) {
    if (cp < kDirectMapSize && g_glyph_of[cp]) return g_glyph_of[cp] - 1u;
    return g_fallback_glyph;
}

// ---------------------------------------------------------------- screen

struct Cell {
    uint32_t cp;
    uint32_t fg;
    uint32_t bg;
    bool operator==(const Cell& o) const { return cp == o.cp && fg == o.fg && bg == o.bg; }
};

constexpr uint32_t kDefaultFg = 0xD8D6E8;
constexpr uint32_t kDefaultBg = 0x15131E;

// ANSI palette (30–37 / 90–97).
constexpr uint32_t kPalette[16] = {
    0x15131E, 0xF2567A, 0x7BD88F, 0xFFD866, 0x78A9FF, 0xD69CFF, 0x5AD4E6, 0xD8D6E8,
    0x5C5873, 0xFF7A99, 0xA2F0B4, 0xFFE59A, 0xA6C6FF, 0xE7C2FF, 0x9AE9F2, 0xFFFFFF,
};

bool g_ready = false;
Framebuffer g_fb;
uint32_t g_cols = 0, g_rows = 0;
Cell* g_cells = nullptr;  // what should be on screen
Cell* g_drawn = nullptr;  // what is on screen
uint32_t g_x = 0, g_y = 0;
uint32_t g_fg = kDefaultFg, g_bg = kDefaultBg;
bool g_cursor_visible = true;
bool g_cursor_drawn = false;
bool g_paused = false;  // a display server owns the screen: keep the cells, touch no pixels

// UTF-8 decoder
uint32_t g_utf8_cp = 0;
uint32_t g_utf8_left = 0;

// ANSI escape parser
enum class Esc { None, Escape, Csi };
Esc g_esc = Esc::None;
uint32_t g_params[8];
uint32_t g_param_count = 0;

uint32_t* pixel_row(uint32_t y) { return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g_fb.Address) + y * g_fb.Pitch); }

void paint(uint32_t col, uint32_t row, const Cell& c) {
    if (g_paused) return;
    const uint8_t* glyph = g_font.glyphs + size_t(glyph_for(c.cp)) * g_font.bytes_per_glyph;
    uint32_t px = col * g_font.width;
    for (uint32_t gy = 0; gy < g_font.height; ++gy) {
        uint32_t* out = pixel_row(row * g_font.height + gy) + px;
        const uint8_t* bits = glyph + gy * g_font.bytes_per_row;
        for (uint32_t gx = 0; gx < g_font.width; ++gx) out[gx] = (bits[gx >> 3] & (0x80 >> (gx & 7))) ? c.fg : c.bg;
    }
}

void flush_cell(uint32_t col, uint32_t row) {
    uint32_t i = row * g_cols + col;
    if (g_cells[i] == g_drawn[i]) return;
    paint(col, row, g_cells[i]);
    g_drawn[i] = g_cells[i];
}

void hide_cursor() {
    if (!g_cursor_drawn) return;
    g_cursor_drawn = false;
    uint32_t i = g_y * g_cols + g_x;
    if (g_x < g_cols && g_y < g_rows) {
        paint(g_x, g_y, g_cells[i]);
        g_drawn[i] = g_cells[i];
    }
}

void show_cursor() {
    if (g_paused || !g_cursor_visible || g_x >= g_cols || g_y >= g_rows) return;
    const Cell& c = g_cells[g_y * g_cols + g_x];
    for (uint32_t gy = g_font.height - 2; gy < g_font.height; ++gy) {
        uint32_t* out = pixel_row(g_y * g_font.height + gy) + g_x * g_font.width;
        for (uint32_t gx = 0; gx < g_font.width; ++gx) out[gx] = c.fg;
    }
    g_cursor_drawn = true;
}

void flush_all() {
    for (uint32_t r = 0; r < g_rows; ++r)
        for (uint32_t c = 0; c < g_cols; ++c) flush_cell(c, r);
}

void scroll() {
    mem::move(g_cells, g_cells + g_cols, sizeof(Cell) * g_cols * (g_rows - 1));
    for (uint32_t c = 0; c < g_cols; ++c) g_cells[(g_rows - 1) * g_cols + c] = {' ', g_fg, g_bg};
    flush_all();
}

void newline() {
    g_x = 0;
    if (++g_y >= g_rows) {
        g_y = g_rows - 1;
        scroll();
    }
}

void put_visible(uint32_t cp) {
    if (g_x >= g_cols) newline();
    g_cells[g_y * g_cols + g_x] = {cp, g_fg, g_bg};
    flush_cell(g_x, g_y);
    ++g_x;
}

void clear_cells(uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; ++i) g_cells[i] = {' ', g_fg, g_bg};
}

void apply_sgr() {
    if (g_param_count == 0) g_params[g_param_count++] = 0;
    for (uint32_t i = 0; i < g_param_count; ++i) {
        uint32_t p = g_params[i];
        if (p == 0) g_fg = kDefaultFg, g_bg = kDefaultBg;
        else if (p >= 30 && p <= 37) g_fg = kPalette[p - 30];
        else if (p >= 90 && p <= 97) g_fg = kPalette[p - 90 + 8];
        else if (p >= 40 && p <= 47) g_bg = kPalette[p - 40];
        else if (p >= 100 && p <= 107) g_bg = kPalette[p - 100 + 8];
        else if (p == 39) g_fg = kDefaultFg;
        else if (p == 49) g_bg = kDefaultBg;
        else if (p == 1 && g_fg == kDefaultFg) g_fg = kPalette[15];
    }
}

void finish_csi(uint8_t final) {
    switch (final) {
        case 'm':
            apply_sgr();
            break;
        case 'J':  // 2J: clear screen
            clear_cells(0, g_cols * g_rows);
            flush_all();
            break;
        case 'K':  // clear to end of line
            clear_cells(g_y * g_cols + g_x, (g_y + 1) * g_cols);
            for (uint32_t c = g_x; c < g_cols; ++c) flush_cell(c, g_y);
            break;
        case 'H': {  // ESC[row;colH, 1-based
            uint32_t r = g_param_count > 0 && g_params[0] ? g_params[0] - 1 : 0;
            uint32_t c = g_param_count > 1 && g_params[1] ? g_params[1] - 1 : 0;
            g_y = r < g_rows ? r : g_rows - 1;
            g_x = c < g_cols ? c : g_cols - 1;
            break;
        }
    }
}

// Returns true when the byte was consumed by the escape parser.
bool escape(uint8_t b) {
    switch (g_esc) {
        case Esc::None:
            if (b != 0x1B) return false;
            g_esc = Esc::Escape;
            return true;
        case Esc::Escape:
            if (b == '[') {
                g_esc = Esc::Csi;
                g_param_count = 0;
                g_params[0] = 0;
            } else {
                g_esc = Esc::None;
            }
            return true;
        case Esc::Csi:
            if (b >= '0' && b <= '9') {
                if (g_param_count == 0) g_param_count = 1, g_params[0] = 0;
                g_params[g_param_count - 1] = g_params[g_param_count - 1] * 10 + (b - '0');
            } else if (b == ';') {
                if (g_param_count == 0) g_param_count = 1, g_params[0] = 0;
                if (g_param_count < 8) g_params[g_param_count++] = 0;
            } else if (b >= 0x40 && b <= 0x7E) {
                finish_csi(b);
                g_esc = Esc::None;
            }
            return true;
    }
    return false;
}

void control_or_visible(uint32_t cp) {
    switch (cp) {
        case '\n': newline(); break;
        case '\r': g_x = 0; break;
        case '\b': if (g_x > 0) --g_x; break;
        case '\t': do put_visible(' '); while (g_x % 4 != 0 && g_x < g_cols); break;
        default:
            if (cp >= 0x20) put_visible(cp);
    }
}

}  // namespace

bool init(const Framebuffer& fb) {
    if (!fb.Address || fb.Bpp != 32 || !load_font()) return false;
    g_fb = fb;
    g_cols = uint32_t(fb.Width / g_font.width);
    g_rows = uint32_t(fb.Height / g_font.height);
    size_t bytes = sizeof(Cell) * g_cols * g_rows;
    g_cells = static_cast<Cell*>(heap::alloc(bytes));
    g_drawn = static_cast<Cell*>(heap::alloc(bytes));
    if (!g_cells || !g_drawn) return false;
    clear_cells(0, g_cols * g_rows);
    for (uint32_t i = 0; i < g_cols * g_rows; ++i) g_drawn[i] = {0xFFFFFFFF, 0, 0};  // force the first paint
    // Fill the area below/right of the grid too.
    for (uint64_t y = 0; y < fb.Height; ++y) {
        uint32_t* row = pixel_row(uint32_t(y));
        for (uint64_t x = 0; x < fb.Width; ++x) row[x] = kDefaultBg;
    }
    flush_all();
    g_x = g_y = 0;
    g_ready = true;
    show_cursor();
    return true;
}

bool ready() { return g_ready; }

void put_codepoint(uint32_t cp) {
    if (!g_ready) return;
    hide_cursor();
    control_or_visible(cp);
    show_cursor();
}

void put_byte(uint8_t b) {
    if (!g_ready) return;
    if (b == 0x1B || g_esc != Esc::None) {
        // Escapes clear and move: the cursor must not stay painted where it was.
        hide_cursor();
        bool consumed = escape(b);
        if (g_esc == Esc::None) show_cursor();
        if (consumed) return;
    }
    if (b < 0x80) {
        g_utf8_left = 0;
        put_codepoint(b);
    } else if ((b & 0xC0) == 0x80) {
        if (g_utf8_left == 0) return;  // stray continuation byte
        g_utf8_cp = (g_utf8_cp << 6) | (b & 0x3F);
        if (--g_utf8_left == 0) put_codepoint(g_utf8_cp);
    } else if ((b & 0xE0) == 0xC0) {
        g_utf8_cp = b & 0x1F, g_utf8_left = 1;
    } else if ((b & 0xF0) == 0xE0) {
        g_utf8_cp = b & 0x0F, g_utf8_left = 2;
    } else {
        g_utf8_cp = b & 0x07, g_utf8_left = 3;
    }
}

void clear() {
    if (!g_ready) return;
    hide_cursor();
    clear_cells(0, g_cols * g_rows);
    flush_all();
    g_x = g_y = 0;
    show_cursor();
}

void set_colors(uint32_t fg, uint32_t bg) { g_fg = fg, g_bg = bg; }
void reset_colors() { g_fg = kDefaultFg, g_bg = kDefaultBg; }
uint32_t columns() { return g_cols; }
uint32_t rows() { return g_rows; }
uint32_t cursor_column() { return g_x; }
uint32_t cursor_row() { return g_y; }

void set_cursor(uint32_t column, uint32_t row) {
    if (!g_ready) return;
    hide_cursor();
    g_x = column < g_cols ? column : g_cols - 1;
    g_y = row < g_rows ? row : g_rows - 1;
    show_cursor();
}

void set_cursor_visible(bool visible) {
    if (!g_ready) return;
    hide_cursor();
    g_cursor_visible = visible;
    show_cursor();
}

void draw_large(uint32_t column, uint32_t row, const char* text, uint32_t fg, uint32_t bg, uint32_t scale) {
    if (!g_ready || !text || g_paused) return;
    uint32_t x0 = column * g_font.width;
    uint32_t y0 = row * g_font.height;
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p; ++p) {
        uint32_t cp = *p;  // ASCII is enough for titles
        const uint8_t* glyph = g_font.glyphs + size_t(glyph_for(cp)) * g_font.bytes_per_glyph;
        for (uint32_t gy = 0; gy < g_font.height * scale; ++gy) {
            uint32_t py = y0 + gy;
            if (py >= g_fb.Height) return;
            uint32_t* out = pixel_row(py);
            const uint8_t* bits = glyph + (gy / scale) * g_font.bytes_per_row;
            for (uint32_t gx = 0; gx < g_font.width * scale; ++gx) {
                uint32_t px = x0 + gx;
                if (px >= g_fb.Width) break;
                uint32_t bx = gx / scale;
                out[px] = (bits[bx >> 3] & (0x80 >> (bx & 7))) ? fg : bg;
            }
        }
        x0 += g_font.width * scale;
    }
}

void set_paused(bool paused) {
    bool was = g_paused;
    g_paused = paused;
    if (was && !paused) redraw();
}

bool paused() { return g_paused; }

void redraw() {
    if (!g_ready || g_paused) return;
    for (uint32_t i = 0; i < g_cols * g_rows; ++i) g_drawn[i] = {0xFFFFFFFF, 0, 0};
    g_cursor_drawn = false;
    for (uint64_t y = uint64_t(g_rows) * g_font.height; y < g_fb.Height; ++y) {
        uint32_t* row = pixel_row(uint32_t(y));
        for (uint64_t x = 0; x < g_fb.Width; ++x) row[x] = kDefaultBg;
    }
    flush_all();
    show_cursor();
}

}  // namespace neko::console
