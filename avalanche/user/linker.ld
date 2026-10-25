ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x40000000;
 .text : { *(.text.start) *(.text*) *(.rodata*) } :text
 . = ALIGN(4096);
 .data : { *(.data*) } :data
 .bss : { *(COMMON) *(.bss*) } :data
 /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
