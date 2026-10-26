#!/bin/sh
set -eu
# GRUB modules may be installed system-wide or unpacked into a local tool prefix.
GRUB_LIB=${GRUB_LIB:-/usr/lib/grub}
iso_root=${1:-build/iso}
grub_config=${2:-boot/grub/grub.cfg}
output=${3:-build/avalanche.iso}
label=${4:-AVALANCHE}
mkdir -p "$iso_root/boot/grub/i386-pc" "$iso_root/EFI/BOOT"
mkdir -p "$iso_root/licenses"
cp LICENSE "$iso_root/licenses/AVALANCHE-MIT.txt"
cp assets/FONT-LICENSE "$iso_root/licenses/FONT-LICENSE.txt"
cp assets/lucide/LICENSE "$iso_root/licenses/LUCIDE-LICENSE.txt"
cp vendor/nuklear/LICENSE "$iso_root/licenses/NUKLEAR-LICENSE.txt"
cp vendor/bearssl/LICENSE.txt "$iso_root/licenses/BEARSSL-MIT.txt"
cp vendor/elk/LICENSE "$iso_root/licenses/ELK-LICENSE.txt"
cp vendor/elk/AGPL-3.0.txt "$iso_root/licenses/ELK-AGPL-3.0.txt"
cp vendor/printf/LICENSE "$iso_root/licenses/PRINTF-MIT.txt"
cp vendor/nuklear/CATPPUCCIN-LICENSE "$iso_root/licenses/CATPPUCCIN-LICENSE.txt"
cp images/GRUB-COPYRIGHT "$iso_root/licenses/GRUB-COPYRIGHT.txt"
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
