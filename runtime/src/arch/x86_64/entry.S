/* First instructions of the kernel. The kernel is compiled with SSE enabled, so the FPU/SSE
   unit must be switched on before any C++ code runs:
     CR0.EM = 0 (no emulation), CR0.MP = 1, CR4.OSFXSR = 1, CR4.OSXMMEXCPT = 1, fninit. */
.section .text
.global neko_entry
neko_entry:
    movq %cr0, %rax
    andq $~(1 << 2), %rax
    orq $(1 << 1), %rax
    movq %rax, %cr0
    movq %cr4, %rax
    orq $(3 << 9), %rax
    movq %rax, %cr4
    fninit
    andq $-16, %rsp
    xorl %ebp, %ebp
    call kmain
    ud2

.section .note.GNU-stack, "", @progbits
