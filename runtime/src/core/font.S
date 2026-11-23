/* Embeds the console font (PSF1/PSF2) into the kernel image. */
.section .rodata
.balign 16
.global neko_font_psf
.global neko_font_psf_end
neko_font_psf:
    .incbin "runtime/assets/fonts/ter-v16n.psf"
neko_font_psf_end:

.section .note.GNU-stack, "", @progbits
