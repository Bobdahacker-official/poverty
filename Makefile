QEMU ?= qemu-system-x86_64
MEMORY ?= 256
.PHONY: all run ext4-image ext4-check
all:
	$(MAKE) -C magenta
run: all
	$(QEMU) -m $(MEMORY) -boot d -cdrom avalanche/build/magenta.iso -drive file=avalanche/build/magenta-data.img,format=raw,if=ide -nic user,model=e1000
ext4-image:
	sh avalanche/tools/ext4-image.sh create avalanche/build/ext4.img
ext4-check:
	sh avalanche/tools/ext4-image.sh check avalanche/build/ext4.img
