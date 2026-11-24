/* Entry point of every user program. The kernel enters here in ring 3 with
   rdi = argc, rsi = argv (strings on the stack, see neko/process.hpp) and rsp 16-byte aligned. */
.section .text
.global _start
.type _start, @function
_start:
    xorl %ebp, %ebp
    andq $-16, %rsp
    call neko_user_start
    ud2
.size _start, . - _start

/* Every extra thread starts here (thread_create): rdi = ThreadControl*, rsp = top of its stack.
   After the entry function returns it marks the control block done, wakes thread_join (futex) and
   calls thread_exit without touching the stack again, so the joiner may free the stack at once. */
.global neko_user_thread_entry
.type neko_user_thread_entry, @function
neko_user_thread_entry:
    xorl %ebp, %ebp
    andq $-16, %rsp
    movq %rdi, %rbx
    call neko_user_thread_run
    movl $1, 28(%rbx)          /* ThreadControl::done */
    leaq 28(%rbx), %rdi        /* futex_wake(&done, all): thread_join sleeps on it */
    movl $0x7fffffff, %esi
    movl $33, %eax
    syscall
    movl $13, %eax             /* thread_exit */
    syscall
    ud2
.size neko_user_thread_entry, . - neko_user_thread_entry

/* Signal trampoline (registered with sigaction): rdi = signal, rsi = handler, rsp = the SigContext the
   kernel saved (16-byte aligned). The FPU/SSE state of the interrupted code is saved here, the handler
   runs as an ordinary function, and sigreturn puts the interrupted context back. */
.global neko_user_signal_entry
.type neko_user_signal_entry, @function
neko_user_signal_entry:
    movq %rsp, %r12            /* the SigContext (callee-saved: the handler keeps it) */
    subq $512, %rsp
    fxsave (%rsp)
    call *%rsi                 /* handler(sig) — rsp is 16-byte aligned here */
    fxrstor (%rsp)
    movq %r12, %rsp
    movl $70, %eax             /* sigreturn */
    syscall
    ud2
.size neko_user_signal_entry, . - neko_user_signal_entry

.section .note.GNU-stack, "", @progbits
