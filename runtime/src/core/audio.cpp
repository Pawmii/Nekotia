// Sound output: mixer, /dev/audio, synthesizer (neko/audio.hpp).
#include <neko/audio.hpp>
#include <neko/runtime.hpp>
#include <neko/sched.hpp>
#include <neko/speaker.hpp>
#include <neko/vfs.hpp>

namespace neko::audio {
namespace {

using namespace proc;  // error codes

constexpr uint32_t kMaxClients = 4;
constexpr uint32_t kQueue = 64 * 1024;          // per client
constexpr uint32_t kLatencyFrames = kRate / 12;  // ~83 ms ahead of the device
constexpr uint32_t kMaxNotes = 16;

struct Client {
    bool used;
    bool closing;   // closed: plays what it queued, then the slot is free
    uint32_t pid;
    uint32_t refs;
    uint8_t* queue;
    uint32_t head, count;
    uint32_t waiting;
    sync::Semaphore space;
};

struct Synth {
    Note notes[kMaxNotes];
    uint32_t count, index;
    uint32_t t;      // samples into the current note (its rest included)
    float phase, vibrato_phase;
    bool active;
};

sync::SpinLock g_lock;
Backend g_dev;
bool g_attached = false;
uint32_t g_write = 0;  // next byte of the ring the mixer writes
uint32_t g_volume = 60;
float g_gain = 0.36f;
Client g_clients[kMaxClients];
Synth g_synth;
uint64_t g_frames = 0;
uint32_t g_underruns = 0;
float g_sine[1025];

// The stop screen runs with the other CPUs halted, perhaps one of them inside the mixer: no lock then.
struct Guard {
    bool held;
    Guard() : held(!crash::in_progress()) {
        if (held) g_lock.lock();
    }
    ~Guard() {
        if (held) g_lock.unlock();
    }
};

float sine(float turns) {  // sin(2π·turns)
    turns -= float(int64_t(turns));
    if (turns < 0) turns += 1;
    float x = turns * 1024;
    int i = int(x);
    float f = x - float(i);
    return g_sine[i] + (g_sine[i + 1] - g_sine[i]) * f;
}

// One sample of the synthesizer, -1…1.
float synth_sample() {
    Synth& s = g_synth;
    if (!s.active) return 0;
    while (s.index < s.count) {
        const Note& n = s.notes[s.index];
        uint32_t length = uint32_t(n.ms) * (kRate / 1000), rest = uint32_t(n.gap_ms) * (kRate / 1000);
        if (s.t < length) {
            float progress = float(s.t) / float(length);
            float hz = float(n.from_hz) + (float(n.to_hz) - float(n.from_hz)) * progress;
            if (n.vibrato) {
                s.vibrato_phase += 5.5f / float(kRate);
                hz *= 1 + float(n.vibrato) / 1731.f * sine(s.vibrato_phase);
            }
            s.phase += hz / float(kRate);
            if (s.phase > 1) s.phase -= 1;
            float attack = float(s.t) / (0.012f * float(kRate)), release = float(length - s.t) / (0.045f * float(kRate));
            float envelope = (attack < 1 ? attack : 1) * (release < 1 ? release : 1);
            ++s.t;
            // A soft tone: the fundamental and two quiet overtones.
            return envelope * (0.72f * sine(s.phase) + 0.20f * sine(s.phase * 2) + 0.08f * sine(s.phase * 3));
        }
        if (s.t < length + rest) {
            ++s.t;
            return 0;
        }
        ++s.index;
        s.t = 0;
    }
    s.active = false;
    return 0;
}

int16_t clip(float v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : int16_t(v); }

// Mixes `frames` frames into the ring at g_write. Lock held (or the stop screen).
void mix(uint32_t frames) {
    for (uint32_t f = 0; f < frames; ++f) {
        float left = 0, right = 0;
        for (Client& c : g_clients) {
            if (!c.used || c.count < kFrame) continue;
            int16_t l, r;
            for (uint32_t b = 0; b < 2; ++b) {
                reinterpret_cast<uint8_t*>(&l)[b] = c.queue[(c.head + b) % kQueue];
                reinterpret_cast<uint8_t*>(&r)[b] = c.queue[(c.head + 2 + b) % kQueue];
            }
            c.head = (c.head + kFrame) % kQueue;
            c.count -= kFrame;
            left += l;
            right += r;
        }
        float voice = synth_sample() * 0.55f * 32767;
        left = (left + voice) * g_gain;
        right = (right + voice) * g_gain;
        auto* out = reinterpret_cast<int16_t*>(g_dev.ring + g_write);
        out[0] = clip(left);
        out[1] = clip(right);
        g_write = (g_write + kFrame) % g_dev.bytes;
    }
    g_frames += frames;
}

// Keeps the ring kLatencyFrames ahead of the device. Lock held (or the stop screen).
void pump_locked() {
    uint32_t play = g_dev.position(g_dev.context) % g_dev.bytes;
    play -= play % kFrame;
    uint32_t ahead = (g_write + g_dev.bytes - play) % g_dev.bytes;
    if (ahead > g_dev.bytes / 2) {  // the device overtook us: start again just ahead of it
        ++g_underruns;
        g_write = play;
        ahead = 0;
    }
    uint32_t want = kLatencyFrames * kFrame;
    if (ahead < want) mix((want - ahead) / kFrame);
}

void pump() {
    Guard guard;
    pump_locked();
    for (Client& c : g_clients) {
        if (!c.used) continue;
        if (c.closing && c.count < kFrame) {  // closed and played out
            heap::free(c.queue);
            c.used = false;
            continue;
        }
        while (c.waiting && kQueue - c.count >= kQueue / 4) {
            --c.waiting;
            c.space.signal();
        }
    }
}

void mixer(uint64_t) {
    for (;;) {
        sched::sleep_ms(10);
        pump();
        vfs::notify();  // writers polling for room
    }
}

// ---------------------------------------------------------------- /dev/audio

Client* client_of(uint32_t pid) {
    for (Client& c : g_clients)
        if (c.used && c.pid == pid) return &c;
    return nullptr;
}

int64_t dev_open(void*, uint32_t flags) {
    uint32_t pid = proc::current_pid();
    sync::ScopedLock guard(g_lock);
    if (Client* c = client_of(pid)) {
        c->closing = false;
        ++c->refs;
        return 0;
    }
    for (Client& c : g_clients) {
        if (c.used) continue;
        auto* queue = static_cast<uint8_t*>(heap::alloc(kQueue));
        if (!queue) return kErrNoMemory;
        c.used = true;
        c.closing = false;
        c.pid = pid;
        c.refs = 1;
        c.queue = queue;
        c.head = c.count = c.waiting = 0;
        c.space = {};
        return 0;
    }
    return kErrBusy;  // four programs already play
}

void dev_close(void*, uint32_t) {
    sync::ScopedLock guard(g_lock);
    Client* c = client_of(proc::current_pid());
    if (!c || !c->refs || --c->refs) return;
    if (c->count >= kFrame) {  // still playing: the mixer frees it once it is played out
        c->closing = true;
        return;
    }
    heap::free(c->queue);
    c->used = false;
}

int64_t dev_write(void*, uint64_t, const void* buffer, uint64_t length) {
    auto* in = static_cast<const uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < length) {
        Client* c;
        {
            sync::ScopedLock guard(g_lock);
            c = client_of(proc::current_pid());
            if (!c) return done ? int64_t(done) : kErrBadFd;
            uint32_t room = kQueue - c->count;
            uint64_t n = length - done < room ? length - done : room;
            for (uint64_t i = 0; i < n; ++i) c->queue[(c->head + c->count + i) % kQueue] = in[done + i];
            c->count += uint32_t(n);
            done += n;
            if (done == length) break;
            ++c->waiting;
        }
        if (!c->space.wait_interruptible()) return done ? int64_t(done) : kErrInterrupted;
    }
    return int64_t(done);
}

uint32_t dev_poll(void*, uint32_t) {
    sync::ScopedLock guard(g_lock);
    Client* c = client_of(proc::current_pid());
    return !c || kQueue - c->count >= kQueue / 4 ? vfs::kPollOut : 0;
}

int64_t dev_control(void*, uint32_t, uint64_t request, uint64_t arg) {
    switch (request) {
        case kAudioGetVolume: return g_volume;
        case kAudioSetVolume:
            if (arg > 100) return kErrBadArgument;
            set_volume(uint32_t(arg));
            return 0;
        case kAudioGetRate: return kRate;
        case kAudioGetQueued: {
            sync::ScopedLock guard(g_lock);
            Client* c = client_of(proc::current_pid());
            return c ? c->count : 0;
        }
        case kAudioDrain:
            for (;;) {
                {
                    sync::ScopedLock guard(g_lock);
                    Client* c = client_of(proc::current_pid());
                    if (!c || c->count < kFrame) break;
                }
                if (sched::killed()) return kErrInterrupted;
                sched::sleep_ms(10);
            }
            sched::sleep_ms(kLatencyFrames * 1000 / kRate);  // and out of the ring
            return 0;
    }
    return kErrNotSupported;
}

const vfs::DeviceOps kOps = {nullptr, dev_write, nullptr, dev_close, nullptr, dev_open, dev_poll, nullptr, dev_control};

bool interrupts_on() {
    uint64_t flags = cpu::save_and_disable_interrupts();
    cpu::restore_interrupts(flags);
    return flags & (1u << 9);
}

}  // namespace

bool attach(const Backend& backend) {
    if (g_attached || !backend.ring || backend.bytes < 4 * kLatencyFrames * kFrame) return false;
    for (int i = 0; i <= 1024; ++i) g_sine[i] = float(math::sin(double(i) * 6.283185307179586 / 1024));
    g_dev = backend;
    g_write = 0;
    mem::set(g_dev.ring, 0, g_dev.bytes);
    g_attached = true;
    vfs::create_device("/dev/audio", &kOps, nullptr);
    sched::spawn("audio", &mixer, 0, sched::Priority::High);
    return true;
}

bool available() { return g_attached; }
const char* device_name() { return g_attached ? g_dev.name : "none"; }

uint32_t volume() { return g_volume; }

void set_volume(uint32_t percent) {
    if (percent > 100) percent = 100;
    Guard guard;
    g_volume = percent;
    g_gain = float(percent * percent) / 10000.f;
}

Stats stats() {
    Guard guard;
    Stats s{g_frames, g_underruns, 0};
    for (Client& c : g_clients) s.Clients += c.used;
    return s;
}

bool play(const Note* notes, uint32_t count) {
    if (!g_attached) return false;
    Guard guard;
    Synth& s = g_synth;
    s.count = count < kMaxNotes ? count : kMaxNotes;
    for (uint32_t i = 0; i < s.count; ++i) s.notes[i] = notes[i];
    s.index = s.t = 0;
    s.active = s.count > 0;
    return true;
}

bool playing() { return g_synth.active; }

void beep(uint32_t hz, uint32_t ms) {
    if (ms > 10000) ms = 10000;
    if (!g_attached) return speaker::beep(hz, ms);
    if (hz > 20000) hz = 20000;
    Note n{uint16_t(hz), uint16_t(hz), uint16_t(ms), 0, 0};
    play(&n, 1);
    if (interrupts_on() && sched::running()) sched::sleep_ms(ms + kLatencyFrames * 1000 / kRate);
}

void play_stop_tune(bool (*stop)()) {
    if (!g_attached) return speaker::play_stop_tune(stop);
    static const Note kTune[] = {
        {560, 900, 120, 0, 0},     // "mi-"
        {900, 450, 420, 30, 200},  // "-aaw": the sad meow
        {392, 386, 300, 0, 60},    // wah
        {370, 364, 300, 0, 60},    // wah
        {349, 342, 300, 0, 60},    // wah
        {330, 330, 700, 45, 0},    // waaaah…
        {330, 290, 450, 45, 0},    // …and down
    };
    if (crash::in_progress()) {  // nobody else is going to play anything now
        for (Client& c : g_clients) c.count = 0;
    }
    play(kTune, sizeof(kTune) / sizeof(kTune[0]));
    bool self = !interrupts_on() || crash::in_progress();  // the mixer thread may never run again: pump here
    uint32_t tail = 0;
    bool stopped = false;
    while (tail < 60 && !stopped) {  // after the tune: the ring plays out (~120 ms)
        if (self) pump();
        stopped = stop && stop();
        if (self) speaker::wait_us(2000);
        else sched::sleep_ms(2);
        if (!g_synth.active) ++tail;
    }
    if (stopped) {
        Guard guard;
        g_synth.active = false;
    }
    // Nobody may mix again (the stop screen): the device keeps looping the ring, so it must hold silence.
    if (self) mem::set(g_dev.ring, 0, g_dev.bytes);
}

}  // namespace neko::audio
