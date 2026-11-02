ENTRY(_start)
SECTIONS {
 . = 1M;
 .multiboot : { KEEP(*(.multiboot)) }
 .text ALIGN(4K) : { *(.text*) }
 .rodata ALIGN(4K) : { *(.rodata*) }
 .data ALIGN(4K) : { *(.data*) }
 .bss ALIGN(4K) : { *(COMMON) *(.bss*) }
 . = ALIGN(4K);
 kernel_end = .;
 ASSERT(kernel_end <= 0x08000000, "Kernel overlaps the Linux compatibility address window")
 /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
