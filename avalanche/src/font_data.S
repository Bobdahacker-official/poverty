.section .rodata
.balign 16
.global font_start,font_end
font_start: .incbin "assets/DejaVuSans.ttf"
font_end:
.section .note.GNU-stack,"",@progbits
