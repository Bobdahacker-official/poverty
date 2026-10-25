/* Uses the real Linux i386 syscall numbers and register ABI, no Avalanche SDK. */
.code32
.section .text
.global _start
_start:
 mov $122,%eax
 mov $uts,%ebx
 int $0x80
 test %eax,%eax
 jnz fail
 mov $5,%eax
 mov $filename,%ebx
 mov $578,%ecx
 mov $420,%edx
 int $0x80
 test %eax,%eax
 js fail
 mov %eax,fd
 mov %eax,%ebx
 mov $4,%eax
 mov $payload,%ecx
 mov $payload_end-payload,%edx
 int $0x80
 cmp $payload_end-payload,%eax
 jne fail
 mov $19,%eax
 mov fd,%ebx
 xor %ecx,%ecx
 xor %edx,%edx
 int $0x80
 test %eax,%eax
 jnz fail
 mov $3,%eax
 mov fd,%ebx
 mov $buffer,%ecx
 mov $payload_end-payload,%edx
 int $0x80
 cmp $payload_end-payload,%eax
 jne fail
 mov $buffer,%esi
 mov $payload,%edi
 mov $payload_end-payload,%ecx
 cld
 repe cmpsb
 jne fail
 mov $6,%eax
 mov fd,%ebx
 int $0x80
 mov $45,%eax
 xor %ebx,%ebx
 int $0x80
 lea 4096(%eax),%ebx
 mov $45,%eax
 int $0x80
 cmp %ebx,%eax
 jne fail
 movl $0x76543210,-4(%eax)
 mov $192,%eax
 xor %ebx,%ebx
 mov $4096,%ecx
 mov $3,%edx
 mov $0x22,%esi
 mov $-1,%edi
 xor %ebp,%ebp
 int $0x80
 test %eax,%eax
 js fail
 movl $0xabcdef01,(%eax)
 mov %eax,%ebx
 mov $4096,%ecx
 mov $91,%eax
 int $0x80
 test %eax,%eax
 jnz fail
 mov $243,%eax
 mov $tls,%ebx
 int $0x80
 test %eax,%eax
 jnz fail
 mov tls,%eax
 shl $3,%eax
 or $3,%eax
 mov %ax,%gs
 mov %gs:0,%eax
 cmp $0x12345678,%eax
 jne fail
 mov $4,%eax
 mov $1,%ebx
 mov $message,%ecx
 mov $message_end-message,%edx
 int $0x80
 xor %ebx,%ebx
 jmp exit
fail:
 mov $4,%eax
 mov $2,%ebx
 mov $failure,%ecx
 mov $failure_end-failure,%edx
 int $0x80
 mov $1,%ebx
exit:
 mov $1,%eax
 int $0x80
.section .rodata
filename: .asciz "linux-data"
payload: .ascii "Linux file I/O persisted.\n"
payload_end:
message: .ascii "Linux i386 ABI: file, memory, TLS, uname OK\n"
message_end:
failure: .ascii "Linux ABI operation failed\n"
failure_end:
.section .data
fd: .long 0
tls: .long -1,tls_value,0xfffff,0x51
tls_value: .long 0x12345678
.section .bss
buffer: .skip 128
uts: .skip 390
.section .note.GNU-stack,"",@progbits
