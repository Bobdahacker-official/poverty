.section .text.start
.code32
.global _start
_start:
 xor %ebp,%ebp
 call main
 mov %eax,%ebx
 xor %eax,%eax
 int $0x80
1: jmp 1b
.section .note.GNU-stack,"",@progbits
