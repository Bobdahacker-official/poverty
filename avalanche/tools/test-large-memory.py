#!/usr/bin/env python3
"""Boot different RAM sizes and verify high physical pages with a real ELF."""
import os
import pathlib
import re
import select
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="avalanche-large-memory-") as directory:
    folder = pathlib.Path(directory)
    obj, executable = folder / "large.o", folder / "large.elf"
    subprocess.run([os.environ.get("CC", "gcc"), "-m64", "-mno-red-zone", "-O2",
                    "-ffreestanding", "-fno-pie", "-fno-stack-protector",
                    "-Iavalanche/include", "-Iavalanche/user", "-c",
                    "avalanche/tools/large-memory-probe.c", "-o", str(obj)],
                   cwd=root, check=True)
    subprocess.run([os.environ.get("LD", "ld"), "-m", "elf_x86_64", "-T",
                    "avalanche/user/linker.ld", "-nostdlib", "-o", str(executable),
                    "avalanche/build/user/crt.o", str(obj),
                    "avalanche/build/user/lib.o"], cwd=root, check=True)
    assert executable.stat().st_size > 65536
    source = folder / "root"
    source.mkdir()
    (source / "large").write_bytes(executable.read_bytes())
    disk = folder / "data.img"
    with disk.open("wb") as file:
        file.truncate(64 * 1024 * 1024)
    subprocess.run(["mkfs.ext4", "-F", "-q", "-b", "4096", "-d", str(source),
                    str(disk)], check=True)
    for memory in (256, 8192, 32768):
        command = [os.environ.get("QEMU", "qemu-system-x86_64"), "-accel", "tcg",
                   "-m", str(memory), "-boot", "d", "-vga", "none", "-display",
                   "none", "-monitor", "none", "-serial", "stdio", "-nic", "none",
                   "-no-reboot", "-cdrom", str(root / "avalanche/build/avalanche.iso"),
                   "-drive", f"file={disk},format=raw,if=ide"]
        for variable, option in [("QEMU_BIOS", "-bios"), ("QEMU_DATA", "-L")]:
            if os.environ.get(variable):
                command.extend([option, os.environ[variable]])
        machine = subprocess.Popen(command, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        pending = bytearray()
        def until(needle):
            deadline = time.monotonic() + 60
            while needle not in pending and time.monotonic() < deadline:
                if select.select([machine.stdout], [], [], .2)[0]:
                    data = os.read(machine.stdout.fileno(), 65536)
                    if not data:
                        break
                    pending.extend(data)
            assert needle in pending, pending.decode(errors="replace")
            end = pending.index(needle) + len(needle)
            output = bytes(pending[:end])
            del pending[:end]
            assert b"KERNEL EXCEPTION" not in output and b"fault " not in output, output
            return output
        def send(text):
            machine.stdin.write((text + "\n").encode())
            machine.stdin.flush()
        try:
            until(b"avalanche> ")
            send("mem")
            before = until(b"avalanche> ")
            managed = int(re.search(rb"Managed RAM: (\d+)", before)[1])
            assert (memory - 160) * 1024 < managed < memory * 1024, before
            send("spawn large")
            output = until(b"LARGE EXECUTABLE AND 16 MIB DATA OK")
            assert b"Spawn failed" not in output, output
            send("mem")
            output = until(b"allocated above 4 GiB: ")
            output += until(b" KiB")
            high = int(re.search(rb"allocated above 4 GiB: (\d+)", output)[1])
            assert high >= 16 * 1024 if memory > 4096 else high == 0, output
            print(f"PASS {memory} MiB RAM: ext4 ELF above 64 KiB, 16 MiB data, "
                  f"{high} KiB allocated above 4 GiB", flush=True)
            until(b"avalanche> ")
            send("run linuxfork")
            output = until(b"avalanche> ")
            assert b"Linux fork, waitpid, exit status and address isolation OK" in output, output
            assert b"Process exit: 0" in output, output
            print(f"PASS {memory} MiB RAM: Linux copy-on-write fork and wait isolation",
                  flush=True)
        finally:
            if machine.poll() is None:
                machine.terminate()
                machine.wait(timeout=5)
