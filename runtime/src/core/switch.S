/* Context switch for kernel threads (System V x86_64; also used by host mode).

   void neko_switch_context(uint64_t* save_rsp, uint64_t new_rsp)
     saves callee-saved registers, RFLAGS (incl. IF) and the FPU/SSE state on the current
     stack, stores rsp to *save_rsp, loads new_rsp and restores the same layout from there.

   Frame (lowest address first), 576 bytes:
     [fxsave area 512] [rflags] [r15] [r14] [r13] [r12] [rbx] [rbp] [return address]  */

.section .text
.global neko_switch_context
.type neko_switch_context, @function
neko_switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushfq
    subq $512, %rsp
    fxsave (%rsp)
    movq %rsp, (%rdi)

    movq %rsi, %rsp
    fxrstor (%rsp)
    addq $512, %rsp
    popfq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size neko_switch_context, . - neko_switch_context

/* First "return" of a new thread lands here: r12 = entry, r13 = argument (set up by sched.cpp). */
.global neko_thread_trampoline
.type neko_thread_trampoline, @function
neko_thread_trampoline:
    movq %r12, %rdi
    movq %r13, %rsi
    call neko_thread_start
    ud2
.size neko_thread_trampoline, . - neko_thread_trampoline

.section .note.GNU-stack, "", @progbits
