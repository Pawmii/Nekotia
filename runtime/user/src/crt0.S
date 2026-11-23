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
   After the entry function returns it marks the control block done and calls thread_exit without
   touching the stack again, so thread_join may free the stack as soon as it sees `done`. */
.global neko_user_thread_entry
.type neko_user_thread_entry, @function
neko_user_thread_entry:
    xorl %ebp, %ebp
    andq $-16, %rsp
    movq %rdi, %rbx
    call neko_user_thread_run
    movl $1, 28(%rbx)          /* ThreadControl::done */
    movl $13, %eax             /* thread_exit */
    syscall
    ud2
.size neko_user_thread_entry, . - neko_user_thread_entry

.section .note.GNU-stack, "", @progbits
