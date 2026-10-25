.section .text
.code32
.global arch_load_gdt
arch_load_gdt:
 mov 4(%esp),%eax
 lgdt (%eax)
 ljmp $8,$1f
1:
 mov $16,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%ss
 mov %ax,%fs
 mov %ax,%gs
 mov $40,%ax
 ltr %ax
 ret
.global irq7_entry,irq15_entry
irq7_entry:
 pushl $0
 pushl $39
 jmp trap_common
irq15_entry:
 pushl $0
 pushl $47
 jmp trap_common
.global timer_entry,syscall_entry
 timer_entry:
 pushl $0
 pushl $32
 jmp trap_common
 syscall_entry:
 pushl $0
 pushl $128
 jmp trap_common
.global trap_common
trap_common:
 cld
 pushal
 push %ds
 push %es
 push %fs
 push %gs
 mov $16,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%fs
 mov %ax,%gs
 mov %esp,%ebx
 and $-16,%esp
 sub $12,%esp
 push %ebx
 call trap_dispatch
 mov %ebx,%esp
 pop %gs
 pop %fs
 pop %es
 pop %ds
 popal
 add $8,%esp
 iret
.global arch_user_enter,arch_user_leave
arch_user_enter:
 push %ebp
 push %ebx
 push %esi
 push %edi
 mov %esp,saved_kernel_esp
 mov 20(%esp),%ecx
 mov 24(%esp),%edx
 mov $35,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%fs
 mov %ax,%gs
 pushl $35
 push %edx
 pushl $0x202
 pushl $27
 push %ecx
 xor %eax,%eax
 xor %ebx,%ebx
 xor %ecx,%ecx
 xor %edx,%edx
 xor %esi,%esi
 xor %edi,%edi
 xor %ebp,%ebp
 iret
arch_user_leave:
 cli
 mov 4(%esp),%edx
 mov $16,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%fs
 mov %ax,%gs
 mov saved_kernel_esp,%esp
 mov %edx,%eax
 pop %edi
 pop %esi
 pop %ebx
 pop %ebp
 sti
 ret
.section .bss
.balign 4
saved_kernel_esp: .skip 4
.section .note.GNU-stack,"",@progbits
