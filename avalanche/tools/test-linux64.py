#!/usr/bin/env python3
"""Run a standard static glibc x86-64 ELF in ring 3, without a custom linker map."""
import os
import pathlib
import select
import subprocess
import tempfile
import time

root=pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="avalanche-linux64-") as directory:
    folder=pathlib.Path(directory); source=folder/"root";source.mkdir()
    subprocess.run([os.environ.get("CC","gcc"),"-static","-O2","-Wall","-Wextra",
                    str(root/"avalanche/tools/linux64-probe.c"),"-o",str(source/"linux64")],check=True)
    disk=folder/"disk.img"
    with disk.open("wb") as f:f.truncate(128*1024**2)
    subprocess.run(["mkfs.ext4","-q","-F","-b","4096","-d",str(source),str(disk)],check=True)
    command=[os.environ.get("QEMU","qemu-system-x86_64"),"-accel","tcg","-m","256",
             "-boot","d","-vga","none","-display","none","-monitor","none","-serial","stdio",
             "-nic","none","-no-reboot","-cdrom",str(root/"avalanche/build/avalanche.iso"),
             "-drive",f"file={disk},format=raw,if=ide",
             "-device","virtio-rng-pci,disable-modern=on"]
    for variable,option in [("QEMU_BIOS","-bios"),("QEMU_DATA","-L")]:
        if os.environ.get(variable):command.extend([option,os.environ[variable]])
    process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    def prompt():
        output=b"";deadline=time.monotonic()+60
        while b"avalanche> " not in output and time.monotonic()<deadline:
            if select.select([process.stdout],[],[],.25)[0]:
                data=os.read(process.stdout.fileno(),65536)
                if not data:break
                output+=data
        assert b"avalanche> " in output,output.decode(errors="replace")
        assert b"KERNEL EXCEPTION" not in output,output.decode(errors="replace")
        return output
    try:
        prompt()
        process.stdin.write(b"run linux64 alpha beta\n");process.stdin.flush()
        output=prompt()
        print(output.decode(errors="replace"),flush=True)
        assert b"LINUX64 COMPLETE" in output,output.decode(errors="replace")
        assert b"Process exit: 0" in output,output.decode(errors="replace")
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)
    subprocess.run(["e2fsck","-fn",str(disk)],check=True,stdout=subprocess.DEVNULL)
    print("PASS Linux64 writes leave ext4 clean")
