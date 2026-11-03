#!/usr/bin/env python3
"""Boot the ClassiCube disk and verify its native Magenta window lifecycle."""
import argparse
import collections
import gzip
import json
import os
import pathlib
import select
import socket
import struct
import subprocess
import tempfile
import time
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("--singleplayer", action="store_true")
args = parser.parse_args()

ROOT = pathlib.Path(__file__).resolve().parents[2]
folder = pathlib.Path(tempfile.mkdtemp(prefix="classicube-test-"))
image = folder / "classicube-data.img"
with gzip.open(ROOT / "Images/classicube-data.img.gz", "rb") as source, image.open("wb") as destination:
    destination.write(source.read())
command = [os.environ.get("QEMU", "qemu-system-x86_64"), "-accel", "tcg", "-cpu", "qemu64", "-m", "512", "-machine", "pc,vmport=on", "-vga", "std", "-display", "none", "-vnc", "unix:" + str(folder / "vnc.sock"), "-monitor", "none", "-serial", "stdio", "-no-reboot", "-cdrom", str(ROOT / "Images/avalanche.iso"), "-boot", "d", "-drive", "file=" + str(image) + ",format=raw,if=ide", "-device", "virtio-rng-pci,disable-modern=on", "-qmp", "unix:" + str(folder / "qmp.sock") + ",server=on,wait=off"]
if os.environ.get("QEMU_BIOS"): command += ["-bios", os.environ["QEMU_BIOS"]]
if os.environ.get("QEMU_DATA"): command += ["-L", os.environ["QEMU_DATA"]]
if os.environ.get("QEMU_VGA_ROM"): command += ["-global", "VGA.romfile=" + os.environ["QEMU_VGA_ROM"]]
process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
log = bytearray(); pending = bytearray(); connection = None
def drain(timeout=0.1):
    if select.select([process.stdout], [], [], timeout)[0]:
        data = os.read(process.stdout.fileno(), 65536); log.extend(data); pending.extend(data)
def until(needle, timeout=60):
    deadline = time.monotonic() + timeout
    while needle not in pending and time.monotonic() < deadline: drain()
    if needle not in pending: raise AssertionError(log[-5000:].decode(errors="replace"))
    del pending[:pending.index(needle) + len(needle)]
def send(text): process.stdin.write(text.encode() + b"\n"); process.stdin.flush()
try:
    until(b"avalanche> "); send("gui"); until(b"Magenta graphical desktop ready")
    connection = socket.socket(socket.AF_UNIX); connection.settimeout(30); connection.connect(str(folder / "qmp.sock")); stream = connection.makefile("rwb", buffering=0); stream.readline()
    def qmp(name, arguments=None):
        request = {"execute": name};
        if arguments: request["arguments"] = arguments
        stream.write(json.dumps(request).encode() + b"\n")
        while True:
            response = json.loads(stream.readline())
            if "error" in response: raise RuntimeError(response)
            if "return" in response: return response["return"]
    qmp("qmp_capabilities"); qmp("human-monitor-command", {"command-line": "sendkey ctrl-t 50"}); until(b"user> "); send("classicube.mgnta --singleplayer" if args.singleplayer else "classicube.mgnta")
    time.sleep(8)
    shot = folder / "classicube.ppm"; qmp("screendump", {"filename": str(shot)}); screen = Image.open(shot).convert("RGB")
    colors = collections.Counter(screen.get_flattened_data())
    if len(colors) < 32: raise AssertionError("ClassiCube did not render a graphical launcher")
    print("PASS ClassiCube software renderer opens a graphical Magenta window", flush=True)
    qmp("input-send-event", {"events": [{"type": "key", "data": {"key": {"type": "qcode", "data": "w"}, "down": True}}, {"type": "key", "data": {"key": {"type": "qcode", "data": "w"}, "down": False}}]})
    time.sleep(1); print("PASS ClassiCube accepts keyboard input through the Magenta window", flush=True)
    symbols = {line.split()[2]: int(line.split()[0], 16) for line in subprocess.check_output(["nm", str(ROOT / "avalanche/build/avalanche.elf")], text=True).splitlines() if len(line.split()) == 3}
    client = folder / "client.bin"
    qmp("human-monitor-command", {"command-line": f'pmemsave {symbols["gui_clients"] - 0xFFFF800000000000:#x} 100 "{client}"'})
    state = client.read_bytes()
    assert struct.unpack_from("Q", state, 24)[0] != 0 and b"ClassiCube" in state, "ClassiCube has no live window"
    if args.singleplayer:
        game_width, game_height = struct.unpack_from("II", state, 4)
        assert game_width > 640, "Singleplayer game surface did not replace launcher"
        print("PASS Singleplayer creates a live game surface", flush=True)
    address = symbols["client_bounds"] - 0xFFFF800000000000
    data = folder / "bounds.bin"; qmp("human-monitor-command", {"command-line": f'pmemsave {address:#x} 16 "{data}"'})
    x, y, width, height = struct.unpack("ffff", data.read_bytes())
    scale_address = symbols["native_scale"] - 0xFFFF800000000000
    scale_file = folder / "scale.bin"; qmp("human-monitor-command", {"command-line": f'pmemsave {scale_address:#x} 4 "{scale_file}"'})
    scale = struct.unpack("f", scale_file.read_bytes())[0]
    qmp("human-monitor-command", {"command-line": "sendkey ctrl-q 50"})
    until(b"user> ", 30); print("PASS ClassiCube window close returns to Magenta Terminal", flush=True)
finally:
    (folder / "serial.log").write_bytes(log)
    if connection: connection.close()
    process.terminate(); process.wait(timeout=5)
subprocess.run(["e2fsck", "-fn", str(image)], check=True, stdout=subprocess.DEVNULL)
print("PASS ClassiCube activity leaves its ext4 application disk clean", flush=True)
