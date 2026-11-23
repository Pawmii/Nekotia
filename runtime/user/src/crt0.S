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

.section .note.GNU-stack, "", @progbits
