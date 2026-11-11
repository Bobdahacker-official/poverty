.section .rodata
.balign 64
.global rtw8821c_firmware
.global rtw8821c_firmware_end
rtw8821c_firmware:
.incbin "vendor/rtw88/rtw8821c_fw.bin"
rtw8821c_firmware_end:
.section .note.GNU-stack,"",@progbits
