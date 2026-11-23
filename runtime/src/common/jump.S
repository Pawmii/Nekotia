/* setjmp/longjmp for C# try/catch (neko/exception.hpp): callee-saved registers, stack pointer,
   return address, MXCSR and the x87 control word. Buffer: uint64_t[10]. */
.section .text
.global neko_setjmp
.type neko_setjmp, @function
neko_setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rdx          /* the caller's rsp after we return */
    movq %rdx, 48(%rdi)
    movq (%rsp), %rdx           /* return address */
    movq %rdx, 56(%rdi)
    stmxcsr 64(%rdi)
    fnstcw 68(%rdi)
    xorl %eax, %eax
    ret
.size neko_setjmp, . - neko_setjmp

.global neko_longjmp
.type neko_longjmp, @function
neko_longjmp:
    ldmxcsr 64(%rdi)
    fldcw 68(%rdi)
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax
1:  jmp *56(%rdi)
.size neko_longjmp, . - neko_longjmp

.section .note.GNU-stack, "", @progbits
