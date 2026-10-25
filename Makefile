QEMU ?= qemu-system-x86_64
MEMORY ?= 256
EXT4_DISK ?=
comma := ,
EXT4_DRIVE = $(if $(EXT4_DISK),-drive file=$(EXT4_DISK)$(comma)format=raw$(comma)if=ide)
.PHONY: all run ext4-image ext4-check test
all:
	$(MAKE) -C magenta
run: all
	$(QEMU) -m $(MEMORY) -boot d -cdrom avalanche/build/magenta.iso -drive file=avalanche/build/magenta-data.img,format=raw,if=ide $(EXT4_DRIVE) -nic user,model=e1000
ext4-image:
	sh avalanche/tools/ext4-image.sh create avalanche/build/ext4.img
ext4-check:
	sh avalanche/tools/ext4-image.sh check avalanche/build/ext4.img
test:
	$(MAKE) -C magenta package
	QEMU=$(QEMU) python3 avalanche/tools/test-storage.py
