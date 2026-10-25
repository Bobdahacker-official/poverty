.section .rodata
.balign 16
.global font_start,font_end
font_start: .incbin "assets/DejaVuSansMono.ttf"
font_end:
.section .note.GNU-stack,"",@progbits
