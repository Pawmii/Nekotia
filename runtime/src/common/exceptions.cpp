// C# exceptions: raise / rethrow / type tests (see neko/exception.hpp). Compiled into the kernel, the
// host build and the user runtime; each provides exc::state() and crash::unhandled_exception.
#include <neko/exception.hpp>

namespace neko::crash {
NEKO_NORETURN void unhandled_exception(const char* type, const char* message, const char* file, int32_t line,
                                       const char* function);
}

namespace neko::exc {

void raise(const char* chain, const char* message, const char* file, int32_t line, const char* function) {
    State* s = state();
    Frame* f = s->frames;
    if (!f || f->barrier || s->irq_depth > 0) crash::unhandled_exception(chain, message, file, line, function);
    f->thrown = Thrown{chain, message, file, line, function};
    // Defers, finally blocks and lock releases between here and the try, innermost first.
    // (One that throws again sends its own exception to the same frame — as in C#.)
    while (s->cleanups && s->cleanups != f->cleanups) {
        Cleanup* c = s->cleanups;
        s->cleanups = c->prev;
        c->run(c);
    }
    s->frames = f->prev;
    f->active = false;
    neko_longjmp(f->jmp, 1);
}

void rethrow(const Thrown& t) {
    Thrown copy = t;  // `t` lives in a frame that may be reused by the next catch
    raise(copy.chain, copy.message, copy.file, copy.line, copy.function);
}

bool is(const Thrown& t, const char* type) {
    for (const char* c = t.chain; c && *c;) {
        const char* a = c;
        const char* b = type;
        while (*a && *a == *b) ++a, ++b;
        if (*a == 0 && *b == 0) return true;
        while (*c) ++c;
        ++c;
    }
    return false;
}

}  // namespace neko::exc
