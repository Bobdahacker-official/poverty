.code32
.section .text
.global _start
_start:
 mov $2,%eax
 int $0x80
 test %eax,%eax
 js fail
 jz child
 mov %eax,child_pid
 mov $4,%eax
 mov $1,%ebx
 mov $parent_message,%ecx
 mov $parent_end-parent_message,%edx
 int $0x80
 mov $7,%eax
 mov child_pid,%ebx
 mov $wait_status,%ecx
 xor %edx,%edx
 int $0x80
 cmp child_pid,%eax
 jne fail
 cmpl $1792,wait_status
 jne fail
 cmpl $42,private_value
 jne fail
 mov $4,%eax
 mov $1,%ebx
 mov $success,%ecx
 mov $success_end-success,%edx
 int $0x80
 xor %ebx,%ebx
 jmp exit
child:
 movl $99,private_value
 mov $30000000,%ecx
1: dec %ecx
 jnz 1b
 mov $4,%eax
 mov $1,%ebx
 mov $child_message,%ecx
 mov $child_end-child_message,%edx
 int $0x80
 mov $7,%ebx
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
parent_message: .ascii "Linux parent waiting for child...\n"
parent_end:
child_message: .ascii "Linux child ran in its own address space\n"
child_end:
success: .ascii "Linux fork, waitpid, exit status and address isolation OK\n"
success_end:
failure: .ascii "Linux fork/wait failed\n"
failure_end:
.section .data
private_value: .long 42
child_pid: .long 0
wait_status: .long 0
.section .note.GNU-stack,"",@progbits
