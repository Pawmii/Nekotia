/* Ring 3 ⇄ ring 0 transitions (see neko/process.hpp).

   neko_syscall_entry — target of the `syscall` instruction (MSR LSTAR). The CPU has put the user
   rip in rcx and rflags in r11 and cleared IF (SFMASK); rsp is still the user stack. We swap GS to
   reach the per-CPU record, switch to the thread's kernel stack (gs:8), build a SyscallFrame
   (same layout as an interrupt frame without vector/error code), save the FPU/SSE state and call
   neko_syscall_dispatch(frame) with interrupts enabled, so a long system call can be preempted.

   neko_enter_user(rip, rsp, arg0, arg1) — first entry of a process into ring 3 through iretq. */

.set GS_KERNEL_RSP, 8
.set GS_USER_RSP, 16
.set USER_CS, 0x23
.set USER_SS, 0x1B

.section .text
.global neko_syscall_entry
.type neko_syscall_entry, @function
neko_syscall_entry:
    swapgs
    movq %rsp, %gs:GS_USER_RSP
    movq %gs:GS_KERNEL_RSP, %rsp
    pushq $USER_SS
    pushq %gs:GS_USER_RSP
    pushq %r11                 /* user rflags */
    pushq $USER_CS
    pushq %rcx                 /* user rip */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi            /* proc::SyscallFrame* */
    subq $512, %rsp            /* 20 pushes keep the stack 16-byte aligned */
    fxsave (%rsp)
    xorl %ebp, %ebp            /* stack traces end here */
    sti
    call neko_syscall_dispatch
    cli
    fxrstor (%rsp)
    addq $512, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax                  /* the result, stored by the dispatcher */
    popq %rcx                  /* user rip for sysret */
    addq $8, %rsp              /* cs */
    popq %r11                  /* user rflags for sysret */
    popq %rsp                  /* user rsp (interrupts are off until sysret) */
    swapgs
    sysretq
.size neko_syscall_entry, . - neko_syscall_entry

.global neko_enter_user
.type neko_enter_user, @function
neko_enter_user:
    cli
    pushq $USER_SS
    pushq %rsi                 /* user rsp */
    pushq $0x202               /* rflags: IF */
    pushq $USER_CS
    pushq %rdi                 /* user rip */
    movq %rdx, %rdi
    movq %rcx, %rsi
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    swapgs
    iretq
.size neko_enter_user, . - neko_enter_user

.section .note.GNU-stack, "", @progbits
