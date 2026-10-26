.section .rodata
.balign 16
.global font_start,font_end
font_start: .incbin "assets/DejaVuSans.ttf"
font_end:
.balign 16
.global gui_icons_start
gui_icons_start: .incbin "assets/lucide/icons.argb"
.section .note.GNU-stack,"",@progbits
