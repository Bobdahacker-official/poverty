#!/bin/sh
set -eu
# GRUB modules may be installed system-wide or unpacked into a local tool prefix.
GRUB_LIB=${GRUB_LIB:-/usr/lib/grub}
iso_root=${1:-build/iso}
grub_config=${2:-boot/grub/grub.cfg}
output=${3:-build/avalanche.iso}
label=${4:-AVALANCHE}
mkdir -p "$iso_root/boot/grub/i386-pc" "$iso_root/EFI/BOOT"
cp build/avalanche.elf "$iso_root/boot/"
cp "$grub_config" "$iso_root/boot/grub/grub.cfg"
cat > "$iso_root/early.cfg" <<'CFG'
search --no-floppy --file --set=root /boot/avalanche.elf
set prefix=($root)/boot/grub
configfile /boot/grub/grub.cfg
CFG
grub-mkimage -d "$GRUB_LIB/i386-pc" -O i386-pc-eltorito -p /boot/grub -c "$iso_root/early.cfg" -o "$iso_root/boot/grub/i386-pc/eltorito.img" biosdisk iso9660 normal configfile search search_fs_file multiboot2 all_video gfxterm
grub-mkimage -d "$GRUB_LIB/x86_64-efi" -O x86_64-efi -p /boot/grub -c "$iso_root/early.cfg" -o "$iso_root/EFI/BOOT/BOOTX64.EFI" part_gpt part_msdos fat iso9660 normal configfile search search_fs_file multiboot2 all_video gfxterm
mformat -C -f 2880 -i "$iso_root/efi.img" ::
mcopy -s -i "$iso_root/efi.img" "$iso_root/EFI" ::/
xorriso -as mkisofs -R -J -V "$label" -o "$output" \
    -b boot/grub/i386-pc/eltorito.img -no-emul-boot -boot-load-size 4 \
    -boot-info-table --grub2-boot-info --grub2-mbr "$GRUB_LIB/i386-pc/boot_hybrid.img" \
    --efi-boot efi.img -efi-boot-part --efi-boot-image "$iso_root"
