/* Multiboot2 entry: GRUB supplies the same protected-mode ABI on BIOS and UEFI. */
.section .multiboot,"a"
.balign 8
header:
.long 0xe85250d6, 0, header_end-header
.long -(0xe85250d6 + (header_end-header))
/* Request an RGB framebuffer, optional so serial-only machines can boot. */
.short 5, 1
.long 20
.long 0, 0, 32
.balign 8
.short 0, 0
.long 8
header_end:

.section .bss
.balign 4096
boot_pml4: .skip 4096
boot_pdpt: .skip 4096
boot_pd: .skip 65536
.balign 16
stack_bottom: .skip 65536
stack_top:
.section .text
.code32
.global _start
_start:
 cli
 cld
 mov %eax,%edi
 mov %ebx,%esi
 mov $stack_top,%esp
 mov $0x80000000,%eax
 cpuid
 cmp $0x80000001,%eax
 jb unsupported
 mov $0x80000001,%eax
 cpuid
 test $0x20000000,%edx
 jz unsupported
 mov $boot_pdpt,%eax
 or $3,%eax
 mov %eax,boot_pml4
 xor %ecx,%ecx
1:
 mov %ecx,%eax
 shl $12,%eax
 add $boot_pd,%eax
 or $3,%eax
 mov %eax,boot_pdpt(,%ecx,8)
 inc %ecx
 cmp $16,%ecx
 jne 1b
 xor %ecx,%ecx
2:
 mov %ecx,%eax
 shl $21,%eax
 or $0x83,%eax
 mov %eax,boot_pd(,%ecx,8)
 mov %ecx,%edx
 shr $11,%edx
 mov %edx,boot_pd+4(,%ecx,8)
 inc %ecx
 cmp $8192,%ecx
 jne 2b
 mov %cr4,%eax
 or $0x20,%eax
 mov %eax,%cr4
 mov $boot_pml4,%eax
 mov %eax,%cr3
 mov $0xc0000080,%ecx
 rdmsr
 or $0x100,%eax
 wrmsr
 mov %cr0,%eax
 or $0x80010000,%eax
 mov %eax,%cr0
 lgdt gdt_desc
 ljmp $8,$long_entry
unsupported:
 cli
 hlt
 jmp unsupported
.code64
long_entry:
 mov %edi,%edi
 mov %esi,%esi
 mov $16,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%ss
 xor %eax,%eax
 mov %ax,%fs
 mov %ax,%gs
 mov $stack_top,%rsp
 xor %rbp,%rbp
 call kernel_main
3: cli
 hlt
 jmp 3b
.balign 8
gdt:
 .quad 0,0x00af9a000000ffff,0x00cf92000000ffff
gdt_desc:
 .word 23
 .long gdt
.section .note.GNU-stack,"",@progbits
