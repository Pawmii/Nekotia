// x86_64 hooks for core/sched.cpp.
#include "arch.hpp"

uint64_t neko_sched_new_stack(size_t size) { return neko::vmm::create_stack(size); }  // guard page below

void neko_sched_idle() { __asm__ volatile("sti; hlt" ::: "memory"); }
