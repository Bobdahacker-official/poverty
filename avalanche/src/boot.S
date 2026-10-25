/* Multiboot2 entry: GRUB supplies the same protected-mode ABI on BIOS and UEFI. */
.section .multiboot,"a"
.balign 8
header:
.long 0xe85250d6, 0, header_end-header
.long -(0xe85250d6 + (header_end-header))
/* Request an RGB framebuffer, optional so serial-only machines can boot. */
.short 5, 1
.long 20
.long 1024, 768, 32
.balign 8
.short 0, 0
.long 8
header_end:
.section .bss
.balign 16
stack_bottom: .skip 65536
stack_top:
.section .text
.code32
.global _start
.type _start,@function
_start:
 cli
 cld
 lgdt gdt_desc
 ljmp $0x08, $reload
reload:
 mov $0x10, %cx
 mov %cx, %ds
 mov %cx, %es
 mov %cx, %fs
 mov %cx, %gs
 mov %cx, %ss
 mov $stack_top, %esp
 xor %ebp, %ebp
 sub $8, %esp
 push %ebx
 push %eax
 call kernel_main
1: cli
 hlt
 jmp 1b
.balign 8
gdt:
 .quad 0
 .quad 0x00cf9a000000ffff
 .quad 0x00cf92000000ffff
gdt_desc:
 .word 23
 .long gdt
.section .note.GNU-stack,"",@progbits
.section .text
.macro EXCEPTION num, error=0
.global exception_\num
exception_\num:
 .if !\error
 pushl $0
 .endif
 pushl $\num
 jmp exception_common
.endm
EXCEPTION 0
EXCEPTION 1
EXCEPTION 2
EXCEPTION 3
EXCEPTION 4
EXCEPTION 5
EXCEPTION 6
EXCEPTION 7
EXCEPTION 8,1
EXCEPTION 9
EXCEPTION 10,1
EXCEPTION 11,1
EXCEPTION 12,1
EXCEPTION 13,1
EXCEPTION 14,1
EXCEPTION 15
EXCEPTION 16
EXCEPTION 17,1
EXCEPTION 18
EXCEPTION 19
EXCEPTION 20
EXCEPTION 21,1
EXCEPTION 22
EXCEPTION 23
EXCEPTION 24
EXCEPTION 25
EXCEPTION 26
EXCEPTION 27
EXCEPTION 28
EXCEPTION 29,1
EXCEPTION 30,1
EXCEPTION 31
exception_common:
 cld
 pushal
 mov %esp,%eax
 and $-16,%esp
 sub $12,%esp
 push %eax
 call exception_panic
 cli
2: hlt
 jmp 2b
.section .rodata
.global exception_table
exception_table:
.long exception_0,exception_1,exception_2,exception_3
.long exception_4,exception_5,exception_6,exception_7
.long exception_8,exception_9,exception_10,exception_11
.long exception_12,exception_13,exception_14,exception_15
.long exception_16,exception_17,exception_18,exception_19
.long exception_20,exception_21,exception_22,exception_23
.long exception_24,exception_25,exception_26,exception_27
.long exception_28,exception_29,exception_30,exception_31
