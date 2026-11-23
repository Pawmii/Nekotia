// Minimal C++ ABI support for a freestanding kernel: new/delete on the kernel heap and the stubs clang expects.
#include <neko/runtime.hpp>

void* operator new(size_t size) {
    void* p = neko::heap::alloc(size);
    if (!p) neko::crash::fail(neko::crash::StopCode::OutOfMemory, "operator new: the kernel heap is exhausted");
    return p;
}
void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { neko::heap::free(p); }
void operator delete[](void* p) noexcept { neko::heap::free(p); }
void operator delete(void* p, size_t) noexcept { neko::heap::free(p); }
void operator delete[](void* p, size_t) noexcept { neko::heap::free(p); }

extern "C" {

void* __dso_handle = nullptr;

// Static destructors never run in a kernel.
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

void __cxa_pure_virtual() { neko::panic("pure virtual function called"); }

}  // extern "C"
