CC = gcc
LD = ld
CFLAGS = -m32 -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -msoft-float -Iinclude
.PHONY: all iso clean
all: build/avalanche.elf
build:
	mkdir -p $@
build/boot.o: src/boot.S | build
	$(CC) -m32 -ffreestanding -fno-pie -c $< -o $@
build/kernel.o: src/kernel.c include/avalanche.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/avalanche.elf: build/boot.o build/kernel.o linker.ld
	$(LD) -m elf_i386 -T linker.ld -nostdlib -o $@ build/boot.o build/kernel.o
iso: build/avalanche.iso
build/avalanche.iso: build/avalanche.elf boot/grub/grub.cfg tools/mkiso.sh
	sh tools/mkiso.sh
clean:
	rm -rf build
