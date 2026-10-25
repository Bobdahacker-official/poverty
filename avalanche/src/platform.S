.section .text
.code64
.global arch_load_gdt
arch_load_gdt:
 lgdt (%rdi)
 pushq $8
 lea 1f(%rip),%rax
 push %rax
 lretq
1:
 mov $16,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%ss
 xor %eax,%eax
 mov %ax,%fs
 mov %ax,%gs
 mov $40,%ax
 ltr %ax
 ret
.global exception_0
exception_0:
 pushq $0
 pushq $0
 jmp trap_common
.global exception_1
exception_1:
 pushq $0
 pushq $1
 jmp trap_common
.global exception_2
exception_2:
 pushq $0
 pushq $2
 jmp trap_common
.global exception_3
exception_3:
 pushq $0
 pushq $3
 jmp trap_common
.global exception_4
exception_4:
 pushq $0
 pushq $4
 jmp trap_common
.global exception_5
exception_5:
 pushq $0
 pushq $5
 jmp trap_common
.global exception_6
exception_6:
 pushq $0
 pushq $6
 jmp trap_common
.global exception_7
exception_7:
 pushq $0
 pushq $7
 jmp trap_common
.global exception_8
exception_8:
 pushq $8
 jmp trap_common
.global exception_9
exception_9:
 pushq $0
 pushq $9
 jmp trap_common
.global exception_10
exception_10:
 pushq $10
 jmp trap_common
.global exception_11
exception_11:
 pushq $11
 jmp trap_common
.global exception_12
exception_12:
 pushq $12
 jmp trap_common
.global exception_13
exception_13:
 pushq $13
 jmp trap_common
.global exception_14
exception_14:
 pushq $14
 jmp trap_common
.global exception_15
exception_15:
 pushq $0
 pushq $15
 jmp trap_common
.global exception_16
exception_16:
 pushq $0
 pushq $16
 jmp trap_common
.global exception_17
exception_17:
 pushq $17
 jmp trap_common
.global exception_18
exception_18:
 pushq $0
 pushq $18
 jmp trap_common
.global exception_19
exception_19:
 pushq $0
 pushq $19
 jmp trap_common
.global exception_20
exception_20:
 pushq $0
 pushq $20
 jmp trap_common
.global exception_21
exception_21:
 pushq $21
 jmp trap_common
.global exception_22
exception_22:
 pushq $0
 pushq $22
 jmp trap_common
.global exception_23
exception_23:
 pushq $0
 pushq $23
 jmp trap_common
.global exception_24
exception_24:
 pushq $0
 pushq $24
 jmp trap_common
.global exception_25
exception_25:
 pushq $0
 pushq $25
 jmp trap_common
.global exception_26
exception_26:
 pushq $0
 pushq $26
 jmp trap_common
.global exception_27
exception_27:
 pushq $0
 pushq $27
 jmp trap_common
.global exception_28
exception_28:
 pushq $0
 pushq $28
 jmp trap_common
.global exception_29
exception_29:
 pushq $29
 jmp trap_common
.global exception_30
exception_30:
 pushq $30
 jmp trap_common
.global exception_31
exception_31:
 pushq $0
 pushq $31
 jmp trap_common
.global timer_entry
timer_entry:
 pushq $0
 pushq $32
 jmp trap_common
.global irq7_entry
irq7_entry:
 pushq $0
 pushq $39
 jmp trap_common
.global irq15_entry
irq15_entry:
 pushq $0
 pushq $47
 jmp trap_common
.global syscall_entry
syscall_entry:
 pushq $0
 pushq $128
 jmp trap_common
.global schedule_entry
schedule_entry:
 pushq $0
 pushq $129
 jmp trap_common
.global trap_common
trap_common:
 cld
 push %rax
 push %rcx
 push %rdx
 push %rbx
 pushq $0
 push %rbp
 push %rsi
 push %rdi
 xor %eax,%eax
 mov %ds,%ax
 push %rax
 mov %es,%ax
 push %rax
 mov %fs,%ax
 push %rax
 mov %gs,%ax
 push %rax
 push %r8
 push %r9
 push %r10
 push %r11
 push %r12
 push %r13
 push %r14
 push %r15
 mov $16,%ax
 mov %ax,%ds
 mov %ax,%es
 xor %eax,%eax
 mov %ax,%fs
 mov %ax,%gs
 mov %rsp,%rdi
 and $-16,%rsp
 call trap_dispatch
 mov %rax,%rsp
 pop %r15
 pop %r14
 pop %r13
 pop %r12
 pop %r11
 pop %r10
 pop %r9
 pop %r8
 pop %rax
 mov %ax,%gs
 pop %rax
 mov %ax,%fs
 pop %rax
 mov %ax,%es
 pop %rax
 mov %ax,%ds
 pop %rdi
 pop %rsi
 pop %rbp
 add $8,%rsp
 pop %rbx
 pop %rdx
 pop %rcx
 pop %rax
 add $16,%rsp
 iretq
.section .rodata
.global exception_table
exception_table:
 .quad exception_0
 .quad exception_1
 .quad exception_2
 .quad exception_3
 .quad exception_4
 .quad exception_5
 .quad exception_6
 .quad exception_7
 .quad exception_8
 .quad exception_9
 .quad exception_10
 .quad exception_11
 .quad exception_12
 .quad exception_13
 .quad exception_14
 .quad exception_15
 .quad exception_16
 .quad exception_17
 .quad exception_18
 .quad exception_19
 .quad exception_20
 .quad exception_21
 .quad exception_22
 .quad exception_23
 .quad exception_24
 .quad exception_25
 .quad exception_26
 .quad exception_27
 .quad exception_28
 .quad exception_29
 .quad exception_30
 .quad exception_31
.section .note.GNU-stack,"",@progbits
