#!/bin/sh
set -eu
# GRUB modules may be installed system-wide or unpacked into a local tool prefix.
GRUB_LIB=${GRUB_LIB:-/usr/lib/grub}
mkdir -p build/iso/boot/grub/i386-pc build/iso/EFI/BOOT
cp build/avalanche.elf build/iso/boot/
cp boot/grub/grub.cfg build/iso/boot/grub/
cat > build/early.cfg <<'CFG'
search --no-floppy --file --set=root /boot/avalanche.elf
set prefix=($root)/boot/grub
configfile /boot/grub/grub.cfg
CFG
grub-mkimage -d "$GRUB_LIB/i386-pc" -O i386-pc-eltorito -p /boot/grub -c build/early.cfg -o build/iso/boot/grub/i386-pc/eltorito.img biosdisk iso9660 normal configfile search search_fs_file multiboot2 all_video gfxterm
# EFI loader is self-contained; GRUB exits EFI boot services before entering the kernel.
grub-mkimage -d "$GRUB_LIB/x86_64-efi" -O x86_64-efi -p /boot/grub -c build/early.cfg -o build/iso/EFI/BOOT/BOOTX64.EFI part_gpt part_msdos fat iso9660 normal configfile search search_fs_file multiboot2 all_video gfxterm
mformat -C -f 2880 -i build/iso/efi.img ::
mcopy -s -i build/iso/efi.img build/iso/EFI ::/
xorriso -as mkisofs -R -J -V AVALANCHE -o build/avalanche.iso \
    -b boot/grub/i386-pc/eltorito.img -no-emul-boot -boot-load-size 4 \
    -boot-info-table --grub2-boot-info --grub2-mbr "$GRUB_LIB/i386-pc/boot_hybrid.img" \
    --efi-boot efi.img -efi-boot-part --efi-boot-image build/iso
