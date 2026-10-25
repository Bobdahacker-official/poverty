#!/bin/sh
# Copy Magenta release media, or create a ready-to-run VirtualBox machine.
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
iso=$root_dir/magenta/images/magenta.iso
data=$root_dir/magenta/images/magenta-data.img

usage() {
    printf '%s\n' "Usage: $0 DESTINATION" "       $0 --virtualbox [MACHINE-NAME] [MACHINE-DIRECTORY]"
}

require_media() {
    [ -f "$iso" ] && [ -f "$data" ] || {
        printf '%s\n' 'Magenta release media is missing; run: make -C magenta package' >&2
        exit 1
    }
}

copy_media() {
    destination=$1
    mkdir -p "$destination"
    cp "$iso" "$destination/magenta.iso"
    cp "$data" "$destination/magenta-data.img"
    printf 'Copied Magenta media to %s\n' "$destination"
}

create_virtualbox_machine() {
    machine_name=${1:-Magenta}
    machine_dir=${2:-"$PWD/$machine_name"}
    command -v VBoxManage >/dev/null 2>&1 || {
        printf '%s\n' 'VBoxManage was not found. Install VirtualBox, then run this command again.' >&2
        exit 1
    }
    case $machine_name in
        *[!A-Za-z0-9._-]*|'')
            printf '%s\n' 'Machine name may contain only letters, numbers, dot, underscore and hyphen.' >&2
            exit 1
            ;;
    esac

    mkdir -p "$machine_dir"
    copy_media "$machine_dir"
    vdi="$machine_dir/magenta-data.vdi"
    VBoxManage convertfromraw "$machine_dir/magenta-data.img" "$vdi" --format VDI
    VBoxManage createvm --name "$machine_name" --ostype Other --basefolder "$machine_dir" --register
    VBoxManage modifyvm "$machine_name" --memory 256 --cpus 1 --ioapic on --boot1 dvd --boot2 disk --boot3 none --boot4 none --nic1 nat --nictype1 82540EM --audio none --usb off
    VBoxManage storagectl "$machine_name" --name IDE --add ide --controller PIIX4
    VBoxManage storageattach "$machine_name" --storagectl IDE --port 0 --device 0 --type dvddrive --medium "$machine_dir/magenta.iso"
    VBoxManage storageattach "$machine_name" --storagectl IDE --port 0 --device 1 --type hdd --medium "$vdi"
    printf 'Created VirtualBox machine "%s". Start it with: VBoxManage startvm "%s"\n' "$machine_name" "$machine_name"
}

require_media
case ${1:-} in
    --virtualbox)
        shift
        create_virtualbox_machine "${1:-Magenta}" "${2:-$PWD/${1:-Magenta}}"
        ;;
    -h|--help|'')
        usage
        ;;
    *)
        copy_media "$1"
        ;;
esac
