.section .text
.global _start
_start:
#ifdef PROBE_KERNEL
    movabs $0xffff800000100000,%rax
    mov (%rax),%rax
#elif defined(PROBE_WRITE)
    movb $0x90,_start(%rip)
#else
    sub $16,%rsp
    movb $0xc3,(%rsp)
    call *%rsp
#endif
    mov $0,%eax
    mov $77,%ebx
    int $0x80
1:  jmp 1b
.section .note.GNU-stack,"",@progbits
