.section .text.start
.code64
.global _start
_start:
 xor %rbp,%rbp
 and $-16,%rsp
 call main
 mov %eax,%ebx
 xor %eax,%eax
 int $0x80
1: jmp 1b
.section .note.GNU-stack,"",@progbits
