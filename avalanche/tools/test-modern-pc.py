#!/usr/bin/env python3
"""UEFI/GOP, 32 GiB RAM, and GPT partitions near the end of a 1 TB NVMe disk."""
import hashlib
import json
import os
import pathlib
import re
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import uuid
import zlib

root = pathlib.Path(__file__).resolve().parents[2]
firmware = pathlib.Path(os.environ.get("OVMF_DIR", "/usr/share/OVMF"))
size = 1_000_000_000_000
part_size = 64 * 1024**2
part_start = ((size - 128 * 1024**2) // 1024**2) * 1024**2
with tempfile.TemporaryDirectory(prefix="avalanche-modern-pc-") as directory:
    folder = pathlib.Path(directory)
    source = folder / "root"
    source.mkdir()
    (source / "fixture").write_text("NVME-HIGH-PARTITION\n")
    for name in ("hello", "cat"):
        shutil.copyfile(root / f"avalanche/build/user/{name}.elf", source / name)
    volume = folder / "ext4.img"
    with volume.open("wb") as file:
        file.truncate(part_size)
    subprocess.run(["mkfs.ext4", "-q", "-F", "-b", "4096", "-d", str(source),
                    str(volume)], check=True)
    for native in (512, 4096):
        disk = folder / f"nvme-{native}.img"
        sectors = size // native
        entries = bytearray(128 * 128)
        def entry(index, kind, first, count, name):
            at = index * 128
            entries[at:at+16] = uuid.UUID(kind).bytes_le
            entries[at+16:at+32] = uuid.uuid4().bytes_le
            struct.pack_into("<QQQ", entries, at+32, first, first+count-1, 0)
            text = name.encode("utf-16le")[:72]
            entries[at+56:at+56+len(text)] = text
        entry(0, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", 1024**2//native,
              1024**3//native, "Existing data")
        entry(1, "0FC63DAF-8483-4772-8E79-3D69D8477DE4", part_start//native,
              part_size//native, "Magenta")
        table_sectors = len(entries)//native
        def header(at, backup, table):
            data = bytearray(native)
            struct.pack_into("<8sIIIIQQQQ16sQIII", data, 0, b"EFI PART", 0x10000,
                             92, 0, 0, at, backup, 2+table_sectors,
                             sectors-2-table_sectors, uuid.UUID(int=1).bytes_le,
                             table, 128, 128, zlib.crc32(entries))
            struct.pack_into("<I", data, 16, zlib.crc32(data[:92]))
            return data
        mbr = bytearray(512)
        mbr[450] = 0xee
        struct.pack_into("<II", mbr, 454, 1, min(sectors-1, 0xffffffff))
        mbr[510:512] = b"\x55\xaa"
        sentinel = b"AVALANCHE-PARTITION-BOUNDARY".ljust(512, b"!")
        with disk.open("wb") as file:
            file.truncate(size)
            file.write(mbr)
            file.seek(native); file.write(header(1, sectors-1, 2))
            file.seek(2*native); file.write(entries)
            file.seek((sectors-1-table_sectors)*native); file.write(entries)
            file.seek((sectors-1)*native); file.write(header(sectors-1, 1, sectors-1-table_sectors))
            file.seek(part_start)
            with volume.open("rb") as payload:
                shutil.copyfileobj(payload, file)
            for offset in (part_start-512, part_start+part_size):
                file.seek(offset); file.write(sentinel)
        def metadata_hash():
            with disk.open("rb") as file:
                first = file.read(1024**2)
                file.seek(size-1024**2)
                return hashlib.sha256(first + file.read()).digest()
        before = metadata_hash()
        for boot in range(2):
            variables = folder / "vars.fd"
            shutil.copyfile(firmware / "OVMF_VARS_4M.fd", variables)
            sock = folder / "qmp.sock"
            if sock.exists(): sock.unlink()
            command = [os.environ.get("QEMU", "qemu-system-x86_64"), "-accel", "tcg",
                       "-machine", "q35", "-cpu", "max", "-m", "32768", "-boot", "d",
                       "-display", "none", "-vga", "std", "-serial", "stdio", "-monitor", "none",
                       "-nic", "none", "-no-reboot", "-qmp", f"unix:{sock},server=on,wait=off",
                       "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={firmware}/OVMF_CODE_4M.fd",
                       "-drive", f"if=pflash,format=raw,unit=1,file={variables}",
                       "-cdrom", str(root / "avalanche/build/avalanche.iso"),
                       "-drive", f"file={disk},format=raw,if=none,id=data",
                       "-device", f"nvme,drive=data,serial=MODERN-PC,logical_block_size={native},physical_block_size={native}"]
            if os.environ.get("QEMU_DATA"): command += ["-L", os.environ["QEMU_DATA"]]
            if os.environ.get("QEMU_VGA_ROM"):
                command += ["-global", "VGA.romfile="+os.environ["QEMU_VGA_ROM"]]
            machine = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)
            pending = bytearray()
            def until(needle):
                deadline = time.monotonic()+90
                while needle not in pending and time.monotonic()<deadline:
                    if select.select([machine.stdout], [], [], .2)[0]:
                        data = os.read(machine.stdout.fileno(), 65536)
                        if not data: break
                        pending.extend(data)
                assert needle in pending, pending.decode(errors="replace")
                end = pending.index(needle)+len(needle)
                output = bytes(pending[:end]); del pending[:end]
                assert b"KERNEL EXCEPTION" not in output, output
                return output
            def run(text):
                machine.stdin.write((text+"\n").encode()); machine.stdin.flush()
                return until(b"avalanche> ")
            connection = None
            try:
                until(b"avalanche> ")
                display = run("display")
                assert b"Firmware: UEFI" in display and b"Framebuffer: " in display, display
                memory = run("mem")
                assert int(re.search(rb"Managed RAM: (\d+)", memory)[1]) > 31*1024**2, memory
                inventory = run("devices")
                assert b"NVMe disk " in inventory and b"sectors=1953125000" in inventory, inventory
                mounted = run("fs")
                assert f"at sector {part_start//512}".encode() in mounted, mounted
                assert b"NVME-HIGH-PARTITION" in run("run cat /fixture")
                if not boot:
                    run("write pc-check PERSISTENT-ONE-TB-NVME")
                    assert b"Disks synchronized." in run("sync")
                else:
                    assert b"PERSISTENT-ONE-TB-NVME" in run("run cat /pc-check")
                    machine.stdin.write(b"gui\n"); machine.stdin.flush()
                    until(b"Magenta graphical desktop ready")
                    time.sleep(2)
                    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    connection.settimeout(30); connection.connect(str(sock))
                    stream = connection.makefile("rwb", buffering=0)
                    stream.readline()
                    def qmp(name, args=None):
                        message = {"execute": name}
                        if args: message["arguments"] = args
                        stream.write(json.dumps(message).encode()+b"\n")
                        while True:
                            reply = json.loads(stream.readline())
                            assert "error" not in reply, reply
                            if "return" in reply: return reply["return"]
                    qmp("qmp_capabilities")
                    screenshot = folder / "gop.ppm"
                    qmp("screendump", {"filename": str(screenshot)})
                    magic, dimensions, maximum, pixels = screenshot.read_bytes().split(b"\n", 3)
                    width, height = map(int, dimensions.split())
                    assert magic == b"P6" and width>=640 and height>=480 and maximum==b"255"
                    assert len(pixels)==width*height*3 and pixels.count(b"\xe6\x00\x7e")>20
                    print(f"PASS UEFI GOP desktop {width}x{height}, 32 GiB RAM, 1 TB NVMe ({native}-byte sectors)", flush=True)
            finally:
                if connection: connection.close()
                if machine.poll() is None: machine.terminate(); machine.wait(timeout=5)
        assert metadata_hash() == before
        with disk.open("rb") as file:
            for offset in (part_start-512, part_start+part_size):
                file.seek(offset); assert file.read(512)==sentinel
            file.seek(part_start)
            with volume.open("wb") as output:
                remaining = part_size
                while remaining:
                    data = file.read(min(1024**2, remaining)); output.write(data); remaining -= len(data)
        result = subprocess.run(["e2fsck", "-fn", str(volume)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert result.returncode == 0, result.stdout.decode(errors="replace")
        print(f"PASS GPT persistence, partition boundaries and e2fsck ({native}-byte sectors)", flush=True)
