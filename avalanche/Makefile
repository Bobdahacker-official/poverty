CC = gcc
LD = ld
CFLAGS = -m64 -mno-red-zone -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -msoft-float -Iinclude
SOURCES = pcnet pci_ecam http nvme web_runtime web tls kernel platform devices ahci fs net user linux virtio block_cache ext4 gui gui_canvas gui_toolkit package sha256 font
BEAR_SOURCES = $(wildcard vendor/bearssl/*.c)
BEAR_OBJECTS = $(patsubst vendor/bearssl/%.c,build/bearssl/%.o,$(BEAR_SOURCES))
OBJECTS = $(addprefix build/,$(addsuffix .o,$(SOURCES))) build/boot.o build/platform_asm.o build/embed.o build/font_data.o build/elk.o build/printf.o
APPS = magenta hello cat udp ush http worker linuxhello linuxfork
APP_ELFS = $(addprefix build/user/,$(addsuffix .elf,$(APPS)))
.PHONY: all iso disk magenta clean
all: build/avalanche.elf build/avalanche-data.img
build build/user:
	mkdir -p $@
build/%.o: src/%.c include/avalanche.h include/system.h include/abi.h include/process.h include/magenta.h include/ext4.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/gui.o: src/gui.c include/gui_canvas.h include/gui_style.h include/gui_toolkit.h include/font.h vendor/nuklear/nuklear.h vendor/nuklear/style.h | build
	$(CC) $(CFLAGS) -msse2 -mfpmath=sse -c $< -o $@
build/gui_toolkit.o: src/gui_toolkit.c include/gui_toolkit.h vendor/nuklear/nuklear.h | build
	$(CC) $(CFLAGS) -msse2 -mfpmath=sse -Wno-unused-function -Wno-unused-parameter -c $< -o $@
build/gui_canvas.o: src/gui_canvas.c include/gui_canvas.h include/gui_toolkit.h include/font.h vendor/nuklear/nuklear.h | build
	$(CC) $(CFLAGS) -msse2 -mfpmath=sse -c $< -o $@
build/boot.o: src/boot.S | build
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
build/platform_asm.o: src/platform.S | build
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
build/font_data.o: src/font_data.S assets/DejaVuSans.ttf assets/lucide/icons.argb | build
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
build/user/crt.o: user/crt.S | build/user
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
build/user/lib.o: user/lib.c user/lib.h include/abi.h include/magenta.h | build/user
	$(CC) $(CFLAGS) -Iuser -c $< -o $@
build/user/%.o: user/%.c user/lib.h include/abi.h include/magenta.h | build/user
	$(CC) $(CFLAGS) -Iuser -c $< -o $@
build/user/%.elf: build/user/%.o build/user/crt.o build/user/lib.o user/linker.ld
	$(LD) -m elf_x86_64 -T user/linker.ld -nostdlib -o $@ build/user/crt.o $< build/user/lib.o
build/user/linuxhello.o: user/linuxhello.S | build/user
	$(CC) -m32 -ffreestanding -fno-pie -c $< -o $@
build/user/linuxfork.o: user/linuxfork.S | build/user
	$(CC) -m32 -ffreestanding -fno-pie -c $< -o $@
build/user/linuxhello.elf: build/user/linuxhello.o user/linux.ld
	$(LD) -m elf_i386 -T user/linux.ld -nostdlib -o $@ $<
build/user/linuxfork.elf: build/user/linuxfork.o user/linux.ld
	$(LD) -m elf_i386 -T user/linux.ld -nostdlib -o $@ $<
build/embed.o: user/embed.S $(APP_ELFS)
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
build/bearssl/%.o: vendor/bearssl/%.c vendor/bearssl/config.h
	mkdir -p build/bearssl
	$(CC) $(CFLAGS) -Wno-error -Ivendor/bearssl -c $< -o $@
build/bearssl.a: $(BEAR_OBJECTS)
	ar rcs $@ $(BEAR_OBJECTS)
build/tls.o: include/trust_anchors.h
build/avalanche.elf: $(OBJECTS) build/bearssl.a linker.ld
	$(LD) -m elf_x86_64 -T linker.ld -nostdlib -o $@ $(OBJECTS) build/bearssl.a
iso: build/avalanche.iso
build/avalanche.iso: build/avalanche.elf boot/grub/grub.cfg tools/mkiso.sh
	sh tools/mkiso.sh
disk: build/avalanche-data.img
build/avalanche-data.img: tools/mkdisk.py $(APP_ELFS)
	python3 tools/mkdisk.py $@ $(APP_ELFS)
magenta: build/magenta.iso build/magenta-data.img
build/magenta.iso: build/avalanche.elf ../magenta/boot/grub.cfg tools/mkiso.sh
	sh tools/mkiso.sh build/magenta-iso ../magenta/boot/grub.cfg build/magenta.iso MAGENTA
build/magenta-data.img: tools/mkdisk.py $(APP_ELFS) ../magenta/files/welcome.txt
	python3 tools/mkdisk.py $@ $(APP_ELFS) ../magenta/files/welcome.txt
clean:
	rm -rf build

build/elk.o: vendor/elk/elk.c vendor/elk/elk.h
	$(CC) $(CFLAGS) -msse2 -mfpmath=sse -DNDEBUG -Wno-error -c $< -o $@
build/printf.o: vendor/printf/printf.c vendor/printf/printf.h
	$(CC) $(CFLAGS) -msse2 -mfpmath=sse -c $< -o $@
build/web_runtime.o: src/web_runtime.c
	$(CC) $(CFLAGS) -msse2 -mfpmath=sse -c $< -o $@
build/web.o: src/web.c include/web.h
	$(CC) $(CFLAGS) -msse2 -mfpmath=sse -c $< -o $@
