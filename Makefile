QEMU ?= qemu-system-x86_64
MEMORY ?= 256
EXT4_DISK ?= avalanche/build/ext4.img
comma := ,
EXT4_DRIVE = $(if $(EXT4_DISK),-drive file=$(EXT4_DISK)$(comma)format=raw$(comma)if=ide)
.PHONY: all run ext4-image ext4-check test packages
all:
	$(MAKE) -C magenta
	@if [ -n "$(EXT4_DISK)" ] && [ ! -f "$(EXT4_DISK)" ]; then sh avalanche/tools/ext4-image.sh create "$(EXT4_DISK)" magenta/files; fi
run: all
	$(QEMU) -m $(MEMORY) -boot d -cdrom avalanche/build/magenta.iso -drive file=avalanche/build/magenta-data.img,format=raw,if=ide $(EXT4_DRIVE) -nic user,model=e1000
ext4-image:
	sh avalanche/tools/ext4-image.sh create avalanche/build/ext4.img magenta/files
ext4-check:
	sh avalanche/tools/ext4-image.sh check avalanche/build/ext4.img
packages:
	$(MAKE) -C magenta
	python3 avalanche/tools/package-repository.py
test:
	$(MAKE) -C magenta package
	$(CC) -Iavalanche/include avalanche/src/sha256.c avalanche/tools/test-sha256.c -o avalanche/build/test-sha256
	avalanche/build/test-sha256
	QEMU=$(QEMU) python3 avalanche/tools/test-storage.py
	QEMU=$(QEMU) python3 avalanche/tools/test-gui.py
