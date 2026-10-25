#!/bin/sh
set -eu
action=${1:?Specify create or check}
image=${2:?Specify image path}
case "$image" in -*) exit 2;; esac
case "$action" in
create)
    command -v mkfs.ext4 >/dev/null
    command -v e2fsck >/dev/null
    command -v debugfs >/dev/null
    if [ -e "$image" ]; then
        printf 'Refusing to replace existing image: %s\n' "$image" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$image")"
    staging=$(mktemp -d "$(dirname "$image")/.ext4.XXXXXXXX")
    trap 'rm -f "$staging/disk"; rmdir "$staging"' EXIT HUP INT TERM
    truncate -s 9G "$staging/disk"
    if [ -n "${3:-}" ]; then
        mkfs.ext4 -F -b 4096 -L AVALANCHE -O extent,dir_index,64bit,flex_bg,has_journal -E lazy_itable_init=0,lazy_journal_init=0 -d "$3" "$staging/disk"
    else
        mkfs.ext4 -F -b 4096 -L AVALANCHE -O extent,dir_index,64bit,flex_bg,has_journal -E lazy_itable_init=0,lazy_journal_init=0 "$staging/disk"
    fi
    e2fsck -fn "$staging/disk"
    debugfs -R stats "$staging/disk"
    ln "$staging/disk" "$image"
    ;;
check)
    e2fsck -fn "$image"
    debugfs -R stats "$image"
    ;;
*)
    printf 'Unknown action: %s\n' "$action" >&2
    exit 2
    ;;
esac
