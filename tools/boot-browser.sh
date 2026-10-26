#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
runtime="$repo_root/avalanche/build/browser-session"
novnc_web=${NOVNC_WEB:-/usr/share/novnc}
qemu=${QEMU:-qemu-system-x86_64}
proxy=${WEBSOCKIFY:-websockify}

command -v "$qemu" >/dev/null
command -v "$proxy" >/dev/null
test -f "$novnc_web/vnc.html"
mkdir -p "$runtime/web"
for asset in vnc.html app core vendor defaults.json mandatory.json; do
  if test -e "$novnc_web/$asset"; then
    cp -RL "$novnc_web/$asset" "$runtime/web/"
  fi
done
cp "$repo_root/tools/novnc-mobile.js" "$runtime/web/magenta-mobile.js"
cp "$repo_root/tools/novnc-mobile.css" "$runtime/web/magenta-mobile.css"
python3 - "$runtime/web/vnc.html" <<'PY'
from pathlib import Path
import sys
page = Path(sys.argv[1])
content = page.read_text()
style = '<style>#noVNC_container{width:100%;height:100%;margin:auto}#noVNC_container canvas{image-rendering:auto}</style><link rel="stylesheet" href="magenta-mobile.css"><script type="module" src="magenta-mobile.js"></script>'
page.write_text(content.replace('</head>', style + '</head>', 1))
PY
cat > "$runtime/web/index.html" <<'HTML'
<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="0;url=vnc.html?autoconnect=1&amp;resize=scale&amp;quality=9&amp;compression=3">
<title>Magenta desktop</title>
<a href="vnc.html?autoconnect=1&amp;resize=scale&amp;quality=9&amp;compression=3">Open Magenta desktop</a>
</html>
HTML

running() {
  local process_id
  process_id=$(cat "$1" 2>/dev/null) || return 1
  kill -0 "$process_id" 2>/dev/null || return 1
  [[ $(ps -p "$process_id" -o stat=) != Z* ]]
}

stop_service() {
  local pid_file=$1 process_id
  running "$pid_file" || return 0
  process_id=$(cat "$pid_file" 2>/dev/null) || return 0
  if [[ $(ps -p "$process_id" -o pgid= | tr -d ' ') == "$process_id" ]]; then
    kill -- "-$process_id"
  else
    pkill -TERM -P "$process_id" 2>/dev/null || true
    kill "$process_id"
  fi
  for attempt in {1..50}; do
    running "$pid_file" || break
    sleep 0.1
  done
  if running "$pid_file"; then
    printf 'The previous service is still shutting down. Run the launcher again.\n' >&2
    exit 1
  fi
  rm -f "$pid_file"
}

case ${1:-} in
  --restart)
    stop_service "$runtime/websockify.pid"
    stop_service "$runtime/qemu.pid"
    ;;
  --restart-vnc)
    stop_service "$runtime/websockify.pid"
    ;;
  '') ;;
  *) printf 'Usage: %s [--restart | --restart-vnc]\n' "$0" >&2; exit 2 ;;
esac

if ! running "$runtime/qemu.pid"; then
  if command -v ss >/dev/null && ss -ltn '( sport = :5901 )' | rg -q 'LISTEN'; then
    printf 'Port 5901 is occupied by another QEMU session; close that session before starting Magenta.\n' >&2
    exit 1
  fi
  if ! test -f "$runtime/data.img"; then
    cp "$repo_root/Images/magenta-data.img" "$runtime/data.img"
  fi
  if ! test -f "$runtime/ext4.img"; then
    stage=$(mktemp -d "$runtime/ext4-files.XXXXXX")
    python3 - "$repo_root/Images/magenta-data.img" "$stage" <<'PYEXTRACT'
from pathlib import Path
import struct, sys
image = Path(sys.argv[1]).read_bytes()
target = Path(sys.argv[2])
start = 2048
records = image[(start + 1) * 512:(start + 5) * 512]
for slot in range(32):
    name, size, _, _, used = struct.unpack_from('<32sIIII', records, slot * 64)
    if used != 1 or not size:
        continue
    name = name.split(b'\0', 1)[0].decode('ascii')
    if not name or '/' in name or name in ('.', '..'):
        continue
    offset = (start + 10 + slot * 256) * 512
    (target / name).write_bytes(image[offset:offset + size])
PYEXTRACT
    command -v mkfs.ext4 >/dev/null || { printf "Install e2fsprogs to create the persistent folder disk.\n" >&2; exit 1; }
    truncate -s 9G "$runtime/ext4.img"
    mkfs.ext4 -q -F -b 4096 -L MAGENTA -O extent,dir_index,64bit,flex_bg,has_journal \
      -E lazy_itable_init=0,lazy_journal_init=0 -d "$stage" "$runtime/ext4.img"
    rm -rf "$stage"
  fi
  acceleration=(-accel tcg,thread=multi -cpu max)
  if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    acceleration=(-accel kvm -cpu host)
  fi
  "$qemu" "${acceleration[@]}" -machine pc,vmport=on -m 256 -boot d \
    -cdrom "$repo_root/Images/magenta.iso" \
    -drive "file=$runtime/data.img,format=raw,if=ide" \
    -drive "file=$runtime/ext4.img,format=raw,if=ide" \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0,disable-modern=on,romfile= \
    -vga std -display none -vnc 127.0.0.1:1,lossy=off \
    -serial "file:$runtime/serial.log" -monitor none \
    -pidfile "$runtime/qemu.pid" -daemonize
fi

if ! running "$runtime/websockify.pid"; then
  if command -v ss >/dev/null && ss -ltn '( sport = :6080 )' | rg -q 'LISTEN'; then
    printf 'Port 6080 is occupied by another viewer; close that session before starting Magenta.\n' >&2
    exit 1
  fi
  nohup setsid "$proxy" --web="$runtime/web" 127.0.0.1:6080 127.0.0.1:5901 \
    > "$runtime/websockify.log" 2>&1 < /dev/null &
  echo "$!" > "$runtime/websockify.pid"
fi

for attempt in {1..25}; do
  if curl --fail --silent --output /dev/null http://127.0.0.1:6080/vnc.html; then
    printf 'Magenta desktop: http://localhost:6080/\n'
    printf 'Repository revision: %s\n' "$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || printf unknown)"
    printf 'Boot image SHA-256: %s\n' "$(sha256sum "$repo_root/Images/magenta.iso" | cut -d ' ' -f 1)"
    exit 0
  fi
  sleep 0.2
done

cat "$runtime/websockify.log" >&2
exit 1
