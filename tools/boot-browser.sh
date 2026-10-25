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
cat > "$runtime/web/index.html" <<'HTML'
<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="0;url=vnc.html?autoconnect=1&amp;resize=scale&amp;quality=4&amp;compression=3">
<title>Magenta desktop</title>
<a href="vnc.html?autoconnect=1&amp;resize=scale&amp;quality=4&amp;compression=3">Open Magenta desktop</a>
</html>
HTML

running() {
  test -f "$1" || return 1
  local process_id
  process_id=$(cat "$1")
  kill -0 "$process_id" 2>/dev/null || return 1
  [[ $(ps -p "$process_id" -o stat=) != Z* ]]
}

if [[ ${1:-} == --restart ]] && running "$runtime/qemu.pid"; then
  kill "$(cat "$runtime/qemu.pid")"
  for attempt in {1..50}; do
    running "$runtime/qemu.pid" || break
    sleep 0.1
  done
  if running "$runtime/qemu.pid"; then
    printf 'The previous VM is still shutting down. Run the launcher again.\n' >&2
    exit 1
  fi
fi

if ! running "$runtime/qemu.pid"; then
  if ! test -f "$runtime/data.img"; then
    cp "$repo_root/Images/magenta-data.img" "$runtime/data.img"
  fi
  acceleration=(-accel tcg,thread=multi -cpu max)
  if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    acceleration=(-accel kvm -cpu host)
  fi
  "$qemu" "${acceleration[@]}" -m 256 -boot d \
    -cdrom "$repo_root/Images/magenta.iso" \
    -drive "file=$runtime/data.img,format=raw,if=ide" \
    -netdev user,id=net0 -device e1000,netdev=net0,romfile= \
    -vga std -display none -vnc 127.0.0.1:1,lossy=on \
    -serial "file:$runtime/serial.log" -monitor none \
    -pidfile "$runtime/qemu.pid" -daemonize
fi

if ! running "$runtime/websockify.pid"; then
  nohup "$proxy" --web="$runtime/web" 127.0.0.1:6080 127.0.0.1:5901 \
    > "$runtime/websockify.log" 2>&1 < /dev/null &
  echo "$!" > "$runtime/websockify.pid"
fi

for attempt in {1..25}; do
  if curl --fail --silent --output /dev/null http://127.0.0.1:6080/vnc.html; then
    printf 'Magenta desktop: http://localhost:6080/\n'
    exit 0
  fi
  sleep 0.2
done

cat "$runtime/websockify.log" >&2
exit 1
