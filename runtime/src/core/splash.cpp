// Boot splash (neko/splash.hpp). Everything is computed, nothing is an image: a gradient sky with stars and a
// moon, two rows of hills, the cat (shapes antialiased with 4×4 samples), the name in the console font scaled
// up smoothly (bilinear over the glyph bits, then a soft threshold), a progress bar with a shimmer.
// The scene is composed in a RAM back buffer (framebuffers are slow to read) and copied out by rectangles.
#include <neko/console.hpp>
#include <neko/runtime.hpp>
#include <neko/sched.hpp>
#include <neko/splash.hpp>

namespace neko::splash {
namespace {

constexpr uint32_t kSkyTop = 0x0D0A18, kSkyBottom = 0x2C1F4A, kGlow = 0x6C46B4, kFarHills = 0x1B1430,
                   kNearHills = 0x0B0912, kMoon = 0xFFF1D0, kHead = 0xF4F0FC, kEar = 0xF6A9C8, kEye = 0x2A2140,
                   kNose = 0xF2709E, kWhisker = 0xCFC7E8, kTitleTop = 0xFFFFFF, kTitleBottom = 0xD3C2FF,
                   kMuted = 0x9A90B8, kTrack = 0x2E2647, kFillFrom = 0xF26D9C, kFillTo = 0xA77BFF,
                   kConsoleBg = 0x15131E;

struct Rect {
    int x, y, w, h;
};

sync::SpinLock g_lock;
volatile bool g_active = false;
bool g_animating = false;
Framebuffer g_fb;
uint32_t* g_back = nullptr;  // W×H, the composed scene
int W = 0, H = 0;
float U = 1;                 // layout unit: 1 at 800 pixels high
int g_cx = 0, g_cy = 0;      // the cat's head
int g_title_y = 0, g_sub_y = 0, g_bar_y = 0, g_status_y = 0, g_bar_w = 0, g_bar_h = 0;
uint32_t g_target = 0, g_shown = 0;  // progress in 1/1000
uint32_t g_frame = 0;
float g_eyes = 1;            // 1 open, 0 closed
char g_status[96];

// ---------------------------------------------------------------- colour and maths

uint32_t mix(uint32_t a, uint32_t b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    auto ch = [&](int s) {
        float x = float((a >> s) & 255), y = float((b >> s) & 255);
        return uint32_t(x + (y - x) * t + 0.5f) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float smooth(float a, float b, float v) {
    float t = clamp01((v - a) / (b - a));
    return t * t * (3 - 2 * t);
}
float sqrtf_(float v) { return __builtin_sqrtf(v); }
int imin(int a, int b) { return a < b ? a : b; }
int imax(int a, int b) { return a > b ? a : b; }

uint32_t hash(uint32_t x, uint32_t y) {
    uint32_t h = x * 0x9E3779B1u ^ (y * 0x85EBCA77u + 0x165667B1u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    return h ^ (h >> 15);
}

// ---------------------------------------------------------------- the sky

// Height of a hill row at x: two parabolic bumps on a base line (the higher one wins).
float hill(float x, float base, float c1, float w1, float h1, float c2, float w2, float h2) {
    float a = (x - c1) / w1, b = (x - c2) / w2;
    float top = 0;
    if (a > -1 && a < 1) top = h1 * (1 - a * a);
    if (b > -1 && b < 1 && h2 * (1 - b * b) > top) top = h2 * (1 - b * b);
    return base - top;
}

uint32_t sky(int x, int y) {
    float fx = float(x) + 0.5f, fy = float(y) + 0.5f;
    uint32_t c = mix(kSkyTop, kSkyBottom, fy / float(H));

    // a soft glow behind the cat
    float gx = (fx - float(g_cx)) / (300 * U), gy = (fy - float(g_cy)) / (240 * U);
    float g = 1 - (gx * gx + gy * gy);
    if (g > 0) c = mix(c, kGlow, g * g * 0.42f);

    // the moon: a crescent with a halo
    float mx = float(W) * 0.83f, my = float(H) * 0.17f, mr = 30 * U;
    float dm = sqrtf_((fx - mx) * (fx - mx) + (fy - my) * (fy - my));
    if (dm < mr * 3.2f) c = mix(c, kMoon, 0.10f * (1 - dm / (mr * 3.2f)) * (1 - dm / (mr * 3.2f)));
    float cut = sqrtf_((fx - mx - 13 * U) * (fx - mx - 13 * U) + (fy - my + 8 * U) * (fy - my + 8 * U));
    float moon = clamp01(mr - dm + 0.5f) * clamp01(cut - mr * 0.9f + 0.5f);
    c = mix(c, kMoon, moon);

    // stars: at most one per cell, at a place, size and brightness of its own
    float horizon = float(H) * 0.78f;
    if (fy < horizon && moon == 0) {
        int cell = int(26 * U) < 12 ? 12 : int(26 * U);
        uint32_t h = hash(uint32_t(x / cell), uint32_t(y / cell));
        if (h % 3 == 0) {
            float sx = float((x / cell) * cell) + 2 + float((h >> 8) % uint32_t(cell - 4)) + 0.5f;
            float sy = float((y / cell) * cell) + 2 + float((h >> 16) % uint32_t(cell - 4)) + 0.5f;
            float r = (h & 0x70) == 0 ? 1.7f : 0.95f;
            float bright = (0.30f + float(h >> 24) / 255 * 0.70f) * (1 - fy / horizon * 0.6f);
            float d = sqrtf_((fx - sx) * (fx - sx) + (fy - sy) * (fy - sy));
            c = mix(c, 0xFFFFFF, clamp01(r - d + 0.5f) * bright);
        }
    }

    // hills: a far row, then a near one
    float wf = float(W);
    float far = hill(fx, float(H) * 0.86f, wf * 0.18f, wf * 0.40f, float(H) * 0.10f, wf * 0.70f, wf * 0.45f, float(H) * 0.075f);
    c = mix(c, kFarHills, clamp01(fy - far + 0.5f));
    float near = hill(fx, float(H) * 0.95f, wf * 0.38f, wf * 0.55f, float(H) * 0.09f, wf * 0.95f, wf * 0.35f, float(H) * 0.12f);
    c = mix(c, kNearHills, clamp01(fy - near + 0.5f));
    return c;
}

void paint_sky(Rect r) {
    for (int y = imax(r.y, 0); y < imin(r.y + r.h, H); ++y)
        for (int x = imax(r.x, 0); x < imin(r.x + r.w, W); ++x) g_back[y * W + x] = sky(x, y);
}

// ---------------------------------------------------------------- the cat

enum class Kind : uint8_t { Ellipse, Triangle, Segment };

struct Shape {
    Kind kind;
    float a, b, c, d, e, f;  // ellipse: cx cy rx ry · triangle: 3 points · segment: 2 points, thickness
    uint32_t color;
    float alpha;
};

bool inside(const Shape& s, float x, float y) {
    switch (s.kind) {
        case Kind::Ellipse: {
            float dx = (x - s.a) / s.c, dy = (y - s.b) / s.d;
            return dx * dx + dy * dy <= 1;
        }
        case Kind::Triangle: {
            float d1 = (x - s.c) * (s.b - s.d) - (s.a - s.c) * (y - s.d);
            float d2 = (x - s.e) * (s.d - s.f) - (s.c - s.e) * (y - s.f);
            float d3 = (x - s.a) * (s.f - s.b) - (s.e - s.a) * (y - s.b);
            bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            return !(neg && pos);
        }
        case Kind::Segment: {
            float vx = s.c - s.a, vy = s.d - s.b, wx = x - s.a, wy = y - s.b;
            float t = clamp01((wx * vx + wy * vy) / (vx * vx + vy * vy));
            float dx = wx - vx * t, dy = wy - vy * t;
            return dx * dx + dy * dy <= s.e * s.e * 0.25f;
        }
    }
    return false;
}

constexpr int kMaxShapes = 24;
Shape g_shapes[kMaxShapes];
int g_shape_count = 0;

void add(Kind k, float a, float b, float c, float d, float e, float f, uint32_t color, float alpha = 1) {
    if (g_shape_count < kMaxShapes) g_shapes[g_shape_count++] = {k, a, b, c, d, e, f, color, alpha};
}

// The cat in units around the head's centre (y down), mirrored parts added twice.
void build_cat() {
    g_shape_count = 0;
    for (int side = -1; side <= 1; side += 2) {
        float m = float(side);
        add(Kind::Triangle, m * 70, -14, m * 58, -98, m * 8, -58, kHead);
        add(Kind::Triangle, m * 57, -30, m * 52, -80, m * 22, -55, kEar);
    }
    add(Kind::Ellipse, 0, 0, 78, 64, 0, 0, kHead);
    for (int side = -1; side <= 1; side += 2) {
        float m = float(side);
        add(Kind::Ellipse, m * 50, 24, 12, 7, 0, 0, kEar, 0.55f);                    // blush
        add(Kind::Ellipse, m * 29, 2, 9.5f, 12.5f * (g_eyes < 0.12f ? 0.12f : g_eyes), 0, 0, kEye);
        if (g_eyes > 0.5f) add(Kind::Ellipse, m * 29 + 3, -3, 3.2f, 3.4f, 0, 0, 0xFFFFFF);
        add(Kind::Segment, m * 2, 29, m * 8, 35, 2.6f, 0, kEye, 0.85f);              // the ω mouth
        add(Kind::Segment, m * 8, 35, m * 14, 31, 2.6f, 0, kEye, 0.85f);
        add(Kind::Segment, m * 44, 18, m * 104, 9, 2.2f, 0, kWhisker, 0.9f);
        add(Kind::Segment, m * 44, 28, m * 106, 32, 2.2f, 0, kWhisker, 0.9f);
    }
    add(Kind::Triangle, -8, 19, 8, 19, 0, 28, kNose);
}

Rect cat_box() { return {g_cx - int(118 * U), g_cy - int(104 * U), int(236 * U), int(180 * U)}; }

void paint_cat() {
    build_cat();
    Rect r = cat_box();
    paint_sky(r);
    for (int y = imax(r.y, 0); y < imin(r.y + r.h, H); ++y) {
        for (int x = imax(r.x, 0); x < imin(r.x + r.w, W); ++x) {
            uint32_t c = g_back[y * W + x];
            for (int i = 0; i < g_shape_count; ++i) {
                const Shape& s = g_shapes[i];
                int hits = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        float lx = (float(x - g_cx) + (float(sx) + 0.5f) / 4) / U;
                        float ly = (float(y - g_cy) + (float(sy) + 0.5f) / 4) / U;
                        hits += inside(s, lx, ly);
                    }
                if (hits) c = mix(c, s.color, float(hits) / 16 * s.alpha);
            }
            g_back[y * W + x] = c;
        }
    }
}

// ---------------------------------------------------------------- text

uint32_t next_codepoint(const uint8_t*& p) {
    uint32_t c = *p++;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    c &= c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F;
    for (; extra && (*p & 0xC0) == 0x80; --extra) c = (c << 6) | (*p++ & 0x3F);
    return c;
}

int text_width(const char* s, float scale) {
    uint32_t w, h, bpr;
    console::glyph(' ', w, h, bpr);
    int n = 0;
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(s); *p;) {
        next_codepoint(p);
        ++n;
    }
    return int(float(n * int(w)) * scale);
}

// Text with its top-left at (x, y), `scale` times the console font, a vertical gradient.
void paint_text(const char* s, int x, int y, float scale, uint32_t top, uint32_t bottom) {
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(s); *p;) {
        uint32_t gw, gh, bpr;
        const uint8_t* g = console::glyph(next_codepoint(p), gw, gh, bpr);
        int ow = int(float(gw) * scale), oh = int(float(gh) * scale);
        if (g)
            for (int oy = 0; oy < oh; ++oy) {
                int py = y + oy;
                if (py < 0 || py >= H) continue;
                float v = (float(oy) + 0.5f) / scale - 0.5f;
                int j = v < 0 ? -1 : int(v);
                float fy = v - float(j);
                uint32_t color = mix(top, bottom, float(oy) / float(oh));
                for (int ox = 0; ox < ow; ++ox) {
                    int px = x + ox;
                    if (px < 0 || px >= W) continue;
                    float u = (float(ox) + 0.5f) / scale - 0.5f;
                    int i = u < 0 ? -1 : int(u);
                    float fx = u - float(i);
                    auto raw = [&](int bx, int by) -> bool {
                        if (bx < 0 || by < 0 || bx >= int(gw) || by >= int(gh)) return false;
                        return (g[by * int(bpr) + (bx >> 3)] & (0x80 >> (bx & 7))) != 0;
                    };
                    // Large text is emboldened (each bit also to its right): the font's strokes are one pixel.
                    auto bit = [&](int bx, int by) -> float { return raw(bx, by) || (scale > 2 && raw(bx - 1, by)) ? 1.f : 0.f; };
                    float val = (bit(i, j) * (1 - fx) + bit(i + 1, j) * fx) * (1 - fy) +
                                (bit(i, j + 1) * (1 - fx) + bit(i + 1, j + 1) * fx) * fy;
                    float a = scale > 1.5f ? smooth(0.25f, 0.6f, val) : val;
                    if (a > 0) g_back[py * W + px] = mix(g_back[py * W + px], color, a);
                }
            }
        x += ow;
    }
}

// ---------------------------------------------------------------- the bar and the status line

Rect bar_box() { return {g_cx - g_bar_w / 2 - 2, g_bar_y - 2, g_bar_w + 4, g_bar_h + 4}; }
Rect status_box() { return {g_cx - int(340 * U), g_status_y, int(680 * U), int(20 * U) + 4}; }

// Coverage of a rounded bar [x0, x1] × [y0, y0 + h] (radius h/2) at a pixel centre.
float capsule(float px, float py, float x0, float x1, float y0, float h) {
    float r = h / 2, cy = y0 + r;
    float cx = px < x0 + r ? x0 + r : px > x1 - r ? x1 - r : px;
    if (x1 - x0 < h) cx = (x0 + x1) / 2;
    float d = sqrtf_((px - cx) * (px - cx) + (py - cy) * (py - cy));
    return clamp01(r - d + 0.5f);
}

void paint_bar() {
    Rect r = bar_box();
    paint_sky(r);
    float x0 = float(g_cx - g_bar_w / 2), x1 = x0 + float(g_bar_w), y0 = float(g_bar_y), h = float(g_bar_h);
    float fill = x0 + (x1 - x0) * float(g_shown) / 1000;
    float shine = x0 - 40 * U + float((g_frame * uint32_t(7 * U + 1)) % uint32_t(fill - x0 + 80 * U + 1));
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            float px = float(x) + 0.5f, py = float(y) + 0.5f;
            uint32_t c = mix(g_back[y * W + x], kTrack, capsule(px, py, x0, x1, y0, h));
            if (g_shown > 0) {
                uint32_t f = mix(kFillFrom, kFillTo, (px - x0) / (x1 - x0));
                float glint = 1 - (px - shine) * (px - shine) / (900 * U * U);
                if (glint > 0) f = mix(f, 0xFFFFFF, glint * glint * 0.55f);
                c = mix(c, f, capsule(px, py, x0, fill, y0, h));
            }
            g_back[y * W + x] = c;
        }
}

void paint_status() {
    Rect r = status_box();
    paint_sky(r);
    paint_text(g_status, g_cx - text_width(g_status, 1) / 2, g_status_y, 1, kMuted, kMuted);
}

// ---------------------------------------------------------------- output

void blit(Rect r) {
    int x0 = imax(r.x, 0), x1 = imin(r.x + r.w, W), y0 = imax(r.y, 0), y1 = imin(r.y + r.h, H);
    if (x0 >= x1) return;
    for (int y = y0; y < y1; ++y)
        mem::copy(reinterpret_cast<uint8_t*>(g_fb.Address) + uint64_t(y) * g_fb.Pitch + uint64_t(x0) * 4, g_back + y * W + x0,
                  uint64_t(x1 - x0) * 4);
    console::touch();
}

void paint_all() {
    paint_sky({0, 0, W, H});
    paint_cat();
    float title_scale = 5 * U;
    paint_text("Nekotia", g_cx - text_width("Nekotia", title_scale) / 2, g_title_y, title_scale,
               kTitleTop, kTitleBottom);
    const char* sub = "a C# kernel for x86_64";
    paint_text(sub, g_cx - text_width(sub, 1.25f * U) / 2, g_sub_y, 1.25f * U, kMuted, kMuted);
    paint_bar();
    paint_status();
}

// Blinks now and then; the bar moves toward the target. Under g_lock.
void tick() {
    ++g_frame;
    if (g_shown < g_target) g_shown += (g_target - g_shown + 5) / 6;
    paint_bar();
    blit(bar_box());
    uint32_t t = g_frame % 110;  // ~3.6 s at 30 frames a second
    float eyes = t < 4 ? 1 - float(t) / 3 : t < 8 ? float(t - 4) / 4 : 1;
    eyes = clamp01(eyes);
    if (eyes != g_eyes) {
        g_eyes = eyes;
        paint_cat();
        blit(cat_box());
    }
}

void animation(uint64_t) {
    for (;;) {
        sched::sleep_ms(33);
        sync::ScopedLock guard(g_lock);
        if (!g_active) break;
        tick();
    }
}

bool has_word(const char* s, const char* word) {
    for (const char* p = s; p && *p; ++p) {
        if (p != s && p[-1] != ' ') continue;
        const char* a = p;
        const char* b = word;
        while (*b && *a == *b) ++a, ++b;
        if (!*b && (*a == 0 || *a == ' ' || *a == '=')) return true;
    }
    return false;
}

}  // namespace

void begin(const char* cmdline) {
    if (g_active || !console::ready() || has_word(cmdline, "nosplash") || has_word(cmdline, "test")) return;
    g_fb = console::framebuffer();
    W = int(g_fb.Width), H = int(g_fb.Height);
    if (W < 320 || H < 240) return;
    g_back = static_cast<uint32_t*>(heap::alloc(uint64_t(W) * uint64_t(H) * 4));
    if (!g_back) return;
    U = float(H) / 800;
    if (float(W) / 1100 < U) U = float(W) / 1100;
    if (U < 0.6f) U = 0.6f;
    g_cx = W / 2;
    g_cy = int(float(H) * 0.38f);
    g_title_y = g_cy + int(96 * U);
    g_sub_y = g_title_y + int(16 * 5 * U) + int(8 * U);
    g_bar_w = int(300 * U);
    g_bar_h = int(6 * U) < 4 ? 4 : int(6 * U);
    g_bar_y = g_sub_y + int(48 * U);
    g_status_y = g_bar_y + g_bar_h + int(14 * U);
    str::copy(g_status, "Starting", sizeof(g_status));
    console::set_covered(true);
    g_active = true;
    sync::ScopedLock guard(g_lock);
    paint_all();
    blit({0, 0, W, H});
}

void animate() {
    sync::ScopedLock guard(g_lock);
    if (!g_active || g_animating) return;
    g_animating = true;
    sched::spawn("splash", &animation, 0, sched::Priority::Low);
}

void step(const char* what, uint32_t percent) {
    if (!g_active) return;
    sync::ScopedLock guard(g_lock);
    if (!g_active) return;
    str::copy(g_status, what, sizeof(g_status));
    g_target = (percent > 100 ? 100 : percent) * 10;
    paint_status();
    blit(status_box());
    if (!g_animating) {  // nobody moves the bar: jump
        g_shown = g_target;
        paint_bar();
        blit(bar_box());
    }
}

void finish() {
    if (!g_active) return;
    // A fast machine boots in half a second: the splash stays a little longer so it can be seen, the bar
    // filling up to the end (the animation thread moves it).
    constexpr uint64_t kAtLeastMs = 2400;
    if (g_animating && sched::running()) {
        {
            sync::ScopedLock guard(g_lock);
            g_target = 1000;
            str::copy(g_status, "Ready", sizeof(g_status));
            paint_status();
            blit(status_box());
        }
        for (int i = 0; i < 100 && (time::uptime_ms() < kAtLeastMs || g_shown < 1000); ++i) sched::sleep_ms(33);
    }
    {
        sync::ScopedLock guard(g_lock);
        g_active = false;  // the animation thread stops at its next frame
        g_shown = g_target = 1000;
        str::copy(g_status, "Ready", sizeof(g_status));
        paint_bar();
        paint_status();
        blit({0, 0, W, H});
    }
    bool can_sleep = sched::running();
    if (can_sleep) sched::sleep_ms(250);
    // Fade into the console's background, then the console (and the boot log) is back.
    constexpr int kSteps = 10;
    for (int s = 1; s <= kSteps; ++s) {
        float t = float(s) / kSteps;
        t = t * t;
        for (int y = 0; y < H; ++y) {
            auto* out = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g_fb.Address) + uint64_t(y) * g_fb.Pitch);
            const uint32_t* in = g_back + y * W;
            for (int x = 0; x < W; ++x) out[x] = mix(in[x], kConsoleBg, t);
        }
        console::touch();
        if (can_sleep) sched::sleep_ms(20);
    }
    sync::ScopedLock guard(g_lock);
    heap::free(g_back);
    g_back = nullptr;
    console::set_covered(false);
}

void abort() {
    g_active = false;
    console::set_covered(false);  // also mid-fade
}

bool active() { return g_active; }

void retarget(const Framebuffer& fb) {
    sync::ScopedLock guard(g_lock);
    g_fb = fb;
    if (g_back) blit({0, 0, W, H});
}

}  // namespace neko::splash
