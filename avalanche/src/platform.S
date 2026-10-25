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
 mov %eax,%esp
 pop %gs
 pop %fs
 pop %es
 pop %ds
 popal
 add $8,%esp
 iret
.global schedule_entry
schedule_entry:
 pushl $0
 pushl $129
 jmp trap_common
.section .note.GNU-stack,"",@progbits
