/* Interrupt stubs for all 256 vectors: normalise the stack (vector + error code),
   save GPRs and the FPU/SSE state, call neko_isr_dispatch(frame). Generated layout, see idt.cpp.

   Interrupts that arrive in ring 3 (CS & 3 in the CPU-pushed frame) swap GS on the way in and
   out, so the kernel always runs with GS = its per-CPU record (smp.hpp). */

.section .text

.macro ISR_NOERR num
isr_\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERR num
isr_\num:
    pushq $\num
    jmp isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47
ISR_NOERR 48
ISR_NOERR 49
ISR_NOERR 50
ISR_NOERR 51
ISR_NOERR 52
ISR_NOERR 53
ISR_NOERR 54
ISR_NOERR 55
ISR_NOERR 56
ISR_NOERR 57
ISR_NOERR 58
ISR_NOERR 59
ISR_NOERR 60
ISR_NOERR 61
ISR_NOERR 62
ISR_NOERR 63
ISR_NOERR 64
ISR_NOERR 65
ISR_NOERR 66
ISR_NOERR 67
ISR_NOERR 68
ISR_NOERR 69
ISR_NOERR 70
ISR_NOERR 71
ISR_NOERR 72
ISR_NOERR 73
ISR_NOERR 74
ISR_NOERR 75
ISR_NOERR 76
ISR_NOERR 77
ISR_NOERR 78
ISR_NOERR 79
ISR_NOERR 80
ISR_NOERR 81
ISR_NOERR 82
ISR_NOERR 83
ISR_NOERR 84
ISR_NOERR 85
ISR_NOERR 86
ISR_NOERR 87
ISR_NOERR 88
ISR_NOERR 89
ISR_NOERR 90
ISR_NOERR 91
ISR_NOERR 92
ISR_NOERR 93
ISR_NOERR 94
ISR_NOERR 95
ISR_NOERR 96
ISR_NOERR 97
ISR_NOERR 98
ISR_NOERR 99
ISR_NOERR 100
ISR_NOERR 101
ISR_NOERR 102
ISR_NOERR 103
ISR_NOERR 104
ISR_NOERR 105
ISR_NOERR 106
ISR_NOERR 107
ISR_NOERR 108
ISR_NOERR 109
ISR_NOERR 110
ISR_NOERR 111
ISR_NOERR 112
ISR_NOERR 113
ISR_NOERR 114
ISR_NOERR 115
ISR_NOERR 116
ISR_NOERR 117
ISR_NOERR 118
ISR_NOERR 119
ISR_NOERR 120
ISR_NOERR 121
ISR_NOERR 122
ISR_NOERR 123
ISR_NOERR 124
ISR_NOERR 125
ISR_NOERR 126
ISR_NOERR 127
ISR_NOERR 128
ISR_NOERR 129
ISR_NOERR 130
ISR_NOERR 131
ISR_NOERR 132
ISR_NOERR 133
ISR_NOERR 134
ISR_NOERR 135
ISR_NOERR 136
ISR_NOERR 137
ISR_NOERR 138
ISR_NOERR 139
ISR_NOERR 140
ISR_NOERR 141
ISR_NOERR 142
ISR_NOERR 143
ISR_NOERR 144
ISR_NOERR 145
ISR_NOERR 146
ISR_NOERR 147
ISR_NOERR 148
ISR_NOERR 149
ISR_NOERR 150
ISR_NOERR 151
ISR_NOERR 152
ISR_NOERR 153
ISR_NOERR 154
ISR_NOERR 155
ISR_NOERR 156
ISR_NOERR 157
ISR_NOERR 158
ISR_NOERR 159
ISR_NOERR 160
ISR_NOERR 161
ISR_NOERR 162
ISR_NOERR 163
ISR_NOERR 164
ISR_NOERR 165
ISR_NOERR 166
ISR_NOERR 167
ISR_NOERR 168
ISR_NOERR 169
ISR_NOERR 170
ISR_NOERR 171
ISR_NOERR 172
ISR_NOERR 173
ISR_NOERR 174
ISR_NOERR 175
ISR_NOERR 176
ISR_NOERR 177
ISR_NOERR 178
ISR_NOERR 179
ISR_NOERR 180
ISR_NOERR 181
ISR_NOERR 182
ISR_NOERR 183
ISR_NOERR 184
ISR_NOERR 185
ISR_NOERR 186
ISR_NOERR 187
ISR_NOERR 188
ISR_NOERR 189
ISR_NOERR 190
ISR_NOERR 191
ISR_NOERR 192
ISR_NOERR 193
ISR_NOERR 194
ISR_NOERR 195
ISR_NOERR 196
ISR_NOERR 197
ISR_NOERR 198
ISR_NOERR 199
ISR_NOERR 200
ISR_NOERR 201
ISR_NOERR 202
ISR_NOERR 203
ISR_NOERR 204
ISR_NOERR 205
ISR_NOERR 206
ISR_NOERR 207
ISR_NOERR 208
ISR_NOERR 209
ISR_NOERR 210
ISR_NOERR 211
ISR_NOERR 212
ISR_NOERR 213
ISR_NOERR 214
ISR_NOERR 215
ISR_NOERR 216
ISR_NOERR 217
ISR_NOERR 218
ISR_NOERR 219
ISR_NOERR 220
ISR_NOERR 221
ISR_NOERR 222
ISR_NOERR 223
ISR_NOERR 224
ISR_NOERR 225
ISR_NOERR 226
ISR_NOERR 227
ISR_NOERR 228
ISR_NOERR 229
ISR_NOERR 230
ISR_NOERR 231
ISR_NOERR 232
ISR_NOERR 233
ISR_NOERR 234
ISR_NOERR 235
ISR_NOERR 236
ISR_NOERR 237
ISR_NOERR 238
ISR_NOERR 239
ISR_NOERR 240
ISR_NOERR 241
ISR_NOERR 242
ISR_NOERR 243
ISR_NOERR 244
ISR_NOERR 245
ISR_NOERR 246
ISR_NOERR 247
ISR_NOERR 248
ISR_NOERR 249
ISR_NOERR 250
ISR_NOERR 251
ISR_NOERR 252
ISR_NOERR 253
ISR_NOERR 254
ISR_NOERR 255

isr_common:
    testb $3, 24(%rsp)         /* CS of the interrupted code: [vector][error][rip][cs] */
    jz 1f
    swapgs
1:
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
    movq %rsp, %rdi            /* crash::CpuState* — the GPRs above */
    subq $512, %rsp            /* FPU/SSE state of the interrupted code (stack is 16-byte aligned here) */
    fxsave (%rsp)
    cld
    call neko_isr_dispatch
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
    popq %rax
    addq $16, %rsp
    testb $3, 8(%rsp)          /* [rip][cs]: back to ring 3? */
    jz 2f
    swapgs
2:
    iretq

.section .rodata
.balign 8
.global neko_isr_table
neko_isr_table:
    .quad isr_0, isr_1, isr_2, isr_3, isr_4, isr_5, isr_6, isr_7
    .quad isr_8, isr_9, isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    .quad isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    .quad isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
    .quad isr_32, isr_33, isr_34, isr_35, isr_36, isr_37, isr_38, isr_39
    .quad isr_40, isr_41, isr_42, isr_43, isr_44, isr_45, isr_46, isr_47
    .quad isr_48, isr_49, isr_50, isr_51, isr_52, isr_53, isr_54, isr_55
    .quad isr_56, isr_57, isr_58, isr_59, isr_60, isr_61, isr_62, isr_63
    .quad isr_64, isr_65, isr_66, isr_67, isr_68, isr_69, isr_70, isr_71
    .quad isr_72, isr_73, isr_74, isr_75, isr_76, isr_77, isr_78, isr_79
    .quad isr_80, isr_81, isr_82, isr_83, isr_84, isr_85, isr_86, isr_87
    .quad isr_88, isr_89, isr_90, isr_91, isr_92, isr_93, isr_94, isr_95
    .quad isr_96, isr_97, isr_98, isr_99, isr_100, isr_101, isr_102, isr_103
    .quad isr_104, isr_105, isr_106, isr_107, isr_108, isr_109, isr_110, isr_111
    .quad isr_112, isr_113, isr_114, isr_115, isr_116, isr_117, isr_118, isr_119
    .quad isr_120, isr_121, isr_122, isr_123, isr_124, isr_125, isr_126, isr_127
    .quad isr_128, isr_129, isr_130, isr_131, isr_132, isr_133, isr_134, isr_135
    .quad isr_136, isr_137, isr_138, isr_139, isr_140, isr_141, isr_142, isr_143
    .quad isr_144, isr_145, isr_146, isr_147, isr_148, isr_149, isr_150, isr_151
    .quad isr_152, isr_153, isr_154, isr_155, isr_156, isr_157, isr_158, isr_159
    .quad isr_160, isr_161, isr_162, isr_163, isr_164, isr_165, isr_166, isr_167
    .quad isr_168, isr_169, isr_170, isr_171, isr_172, isr_173, isr_174, isr_175
    .quad isr_176, isr_177, isr_178, isr_179, isr_180, isr_181, isr_182, isr_183
    .quad isr_184, isr_185, isr_186, isr_187, isr_188, isr_189, isr_190, isr_191
    .quad isr_192, isr_193, isr_194, isr_195, isr_196, isr_197, isr_198, isr_199
    .quad isr_200, isr_201, isr_202, isr_203, isr_204, isr_205, isr_206, isr_207
    .quad isr_208, isr_209, isr_210, isr_211, isr_212, isr_213, isr_214, isr_215
    .quad isr_216, isr_217, isr_218, isr_219, isr_220, isr_221, isr_222, isr_223
    .quad isr_224, isr_225, isr_226, isr_227, isr_228, isr_229, isr_230, isr_231
    .quad isr_232, isr_233, isr_234, isr_235, isr_236, isr_237, isr_238, isr_239
    .quad isr_240, isr_241, isr_242, isr_243, isr_244, isr_245, isr_246, isr_247
    .quad isr_248, isr_249, isr_250, isr_251, isr_252, isr_253, isr_254, isr_255

.section .note.GNU-stack, "", @progbits
