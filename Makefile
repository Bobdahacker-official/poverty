QEMU ?= qemu-system-x86_64
.DEFAULT_GOAL := all
MEMORY ?= 256
EXT4_DISK ?= avalanche/build/ext4.img
comma := ,
EXT4_DRIVE = $(if $(EXT4_DISK),-drive file=$(EXT4_DISK)$(comma)format=raw$(comma)if=ide)
.PHONY: all run ext4-image ext4-check test packages package sdk netsurf test-netsurf classicube test-classicube
netsurf: all
	python3 ports/netsurf/build.py
classicube: all
	python3 ports/classicube/build.py
test-classicube: classicube
	QEMU=$(QEMU) python3 ports/classicube/test.py
test-netsurf:
	QEMU=$(QEMU) python3 ports/netsurf/test.py
	QEMU=$(QEMU) python3 ports/netsurf/test.py --tls-only --untrusted
	QEMU=$(QEMU) python3 ports/netsurf/test.py --welcome
sdk:
	$(MAKE) -C sdk
all:
	$(MAKE) -C magenta
	@if [ -n "$(EXT4_DISK)" ] && [ ! -f "$(EXT4_DISK)" ]; then sh avalanche/tools/ext4-image.sh create "$(EXT4_DISK)" magenta/files; fi
run: all
	$(QEMU) -m $(MEMORY) -boot d -cdrom avalanche/build/magenta.iso -drive file=avalanche/build/magenta-data.img,format=raw,if=ide $(EXT4_DRIVE) -nic user,model=e1000 -device virtio-rng-pci,disable-modern=on
ext4-image:
	sh avalanche/tools/ext4-image.sh create avalanche/build/ext4.img magenta/files
ext4-check:
	sh avalanche/tools/ext4-image.sh check avalanche/build/ext4.img
packages:
	$(MAKE) -C magenta
	python3 avalanche/tools/package-repository.py
package:
	$(MAKE) -C magenta package
	$(MAKE) -C avalanche package
	$(MAKE) -C sdk
	$(MAKE) classicube
	sha256sum Images/avalanche.iso Images/magenta.iso Images/avalanche-data.img Images/magenta-data.img Images/magenta-sdk.tar.gz Images/netsurf-data.img.gz Images/classicube-data.img.gz > Images/SHA256SUMS
test:
	$(MAKE) package
	$(CC) -Iavalanche/include avalanche/src/sha256.c avalanche/tools/test-sha256.c -o avalanche/build/test-sha256
	avalanche/build/test-sha256
	$(CC) -O2 -Wall -Wextra -Werror -Iavalanche/include avalanche/src/games.c avalanche/tools/test-games.c -o avalanche/build/test-games
	avalanche/build/test-games
	$(CC) -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -Iavalanche/include avalanche/tools/test-web.c avalanche/src/sha256.c avalanche/src/platform.c avalanche/vendor/elk/elk.c avalanche/vendor/printf/printf.c -lm -o avalanche/build/test-web
	@web_test_dir=$$(mktemp -d); trap 'rm -rf "$$web_test_dir"' EXIT; cd "$$web_test_dir" && "$(CURDIR)/avalanche/build/test-web" "$(CURDIR)/magenta/files/webnotes.html"
	$(CC) -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -Iavalanche/include avalanche/src/devices.c avalanche/tools/test-pci-bars.c -o avalanche/build/test-pci-bars
	avalanche/build/test-pci-bars
	$(CC) -O2 -Wall -Wextra -Werror -Iavalanche/include avalanche/tools/test-usb-storage.c -o avalanche/build/test-usb-storage
	avalanche/build/test-usb-storage
	$(CC) -O2 -Wall -Wextra -Werror -Iavalanche/include avalanche/tools/test-partitions.c -o avalanche/build/test-partitions
	avalanche/build/test-partitions
	python3 avalanche/tools/test-package-security.py
	QEMU=$(QEMU) python3 avalanche/tools/test-sdk.py
	python3 avalanche/tools/test-images.py
	QEMU=$(QEMU) python3 avalanche/tools/test-memory-protection.py
	QEMU=$(QEMU) python3 avalanche/tools/test-linux64.py
	QEMU=$(QEMU) python3 avalanche/tools/test-large-memory.py
	QEMU=$(QEMU) python3 avalanche/tools/test-modern-pc.py
	QEMU=$(QEMU) python3 avalanche/tools/test-entropy.py
	QEMU=$(QEMU) python3 avalanche/tools/test-hardware.py
	QEMU=$(QEMU) python3 avalanche/tools/test-usb-input.py
	QEMU=$(QEMU) python3 avalanche/tools/test-usb-input.py --xhci
	QEMU=$(QEMU) python3 avalanche/tools/test-audio.py
	QEMU=$(QEMU) python3 avalanche/tools/test-audio.py --gui
	QEMU=$(QEMU) python3 avalanche/tools/test-storage.py
	QEMU=$(QEMU) python3 avalanche/tools/test-gui.py
	QEMU=$(QEMU) python3 avalanche/tools/test-image-gui.py
	QEMU=$(QEMU) python3 avalanche/tools/test-image-gui.py --ext4
	$(MAKE) test-netsurf
	$(MAKE) test-classicube
