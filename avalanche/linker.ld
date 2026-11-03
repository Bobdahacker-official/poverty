ENTRY(_start)
SECTIONS {
 . = 1M;
 .multiboot : { KEEP(*(.multiboot)) }
 .text ALIGN(4K) : { *(.text*) }
 .rodata ALIGN(4K) : { *(.rodata*) *(.lrodata*) }
 .data ALIGN(4K) : { *(.data*) *(.ldata*) }
 .boot.bss ALIGN(4K) (NOLOAD) : { *(.boot.bss) }
 . = ALIGN(4K);
 kernel_identity_end = .;
 .bss (0xffff800000000000 + kernel_identity_end) (NOLOAD) : AT(kernel_identity_end) { *(COMMON) *(.bss*) *(.lbss*) *(.lcommon) }
 kernel_end = ALIGN(LOADADDR(.bss) + SIZEOF(.bss), 4096);
 ASSERT(kernel_identity_end <= 0x00400000, "Kernel bootstrap overlaps the Linux executable address window")
 ASSERT(kernel_end <= 0x100000000, "Static DMA buffers exceed the 32-bit physical address range")
 /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
