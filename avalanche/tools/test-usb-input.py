#!/usr/bin/env python3
"""Type through a USB keyboard while the root filesystem uses the same hub."""
import os
import pathlib
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="avalanche-usb-input-") as directory:
    folder = pathlib.Path(directory)
    disk, serial = folder / "usb.img", folder / "serial.log"
    with disk.open("wb") as file:
        file.truncate(64 * 1024 * 1024)
    subprocess.run(["mkfs.ext4", "-q", "-F", "-b", "4096", str(disk)], check=True)
    command = [os.environ.get("QEMU", "qemu-system-x86_64"), "-accel", "tcg",
               "-machine", "pc,i8042=off", "-m", "256", "-boot", "d",
               "-vga", "none", "-display", "none", "-nic", "none",
               "-monitor", "stdio", "-serial", "file:" + str(serial),
               "-no-reboot", "-cdrom", str(root / "avalanche/build/avalanche.iso"),
               "-drive", f"file={disk},format=raw,if=none,id=data",
               "-device", "piix3-usb-uhci,id=usb",
               "-device", "usb-hub,bus=usb.0,port=1",
               "-device", "usb-storage,drive=data,bus=usb.0,port=1.1",
               "-device", "usb-kbd,bus=usb.0,port=1.2"]
    for variable, option in [("QEMU_BIOS", "-bios"), ("QEMU_DATA", "-L")]:
        if os.environ.get(variable):
            command.extend([option, os.environ[variable]])
    machine = subprocess.Popen(command, stdin=subprocess.PIPE,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    offset = 0
    def until(needle):
        global offset
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            output = serial.read_bytes() if serial.exists() else b""
            if needle in output[offset:]:
                offset = output.index(needle, offset) + len(needle)
                assert b"KERNEL EXCEPTION" not in output, output
                return output
            if machine.poll() is not None:
                raise AssertionError(machine.stderr.read().decode(errors="replace"))
            time.sleep(.1)
        raise AssertionError(output.decode(errors="replace"))
    def type_command(text):
        for character in text + "\n":
            key = {" ": "spc", "-": "minus", "\n": "ret"}.get(character, character)
            machine.stdin.write(("sendkey " + key + " 50\n").encode())
            machine.stdin.flush()
            time.sleep(.12)
    try:
        output = until(b"avalanche> ")
        assert b"USB keyboard ready" in output and b"USB mass-storage ready" in output
        type_command("echo usb-keyboard")
        until(b"\r\nusb-keyboard\r\n")
        until(b"avalanche> ")
        print("PASS USB keyboard input with the PS/2 controller disabled", flush=True)
        type_command("write usb-input usb-keyboard-write")
        # Deliberately type while the allocation/journal writes are in progress.
        type_command("cat usb-input")
        until(b"\r\nusb-keyboard-write\r\n")
        until(b"avalanche> ")
        type_command("sync")
        until(b"Disks synchronized.")
        print("PASS USB keyboard and persistent disk I/O share a hub", flush=True)
    finally:
        if machine.poll() is None:
            machine.stdin.write(b"quit\n")
            machine.stdin.flush()
            try:
                machine.wait(timeout=5)
            except subprocess.TimeoutExpired:
                machine.kill()
                machine.wait()
    result = subprocess.run(["e2fsck", "-fn", str(disk)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    assert result.returncode == 0, result.stdout.decode(errors="replace")
    print("PASS USB keyboard-driven writes leave ext4 clean", flush=True)
