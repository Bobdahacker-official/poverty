.section .rodata
.balign 16
.global app_hello_start,app_hello_end
app_hello_start: .incbin "build/user/hello.elf"
app_hello_end:
.balign 16
.global app_cat_start,app_cat_end
app_cat_start: .incbin "build/user/cat.elf"
app_cat_end:
.balign 16
.global app_udp_start,app_udp_end
app_udp_start: .incbin "build/user/udp.elf"
app_udp_end:
.balign 16
.global app_ush_start,app_ush_end
app_ush_start: .incbin "build/user/ush.elf"
app_ush_end:
.section .note.GNU-stack,"",@progbits
.section .rodata
.balign 16
.global app_http_start,app_http_end
app_http_start: .incbin "build/user/http.elf"
app_http_end:
