#!/usr/bin/env python3
"""Exercise the native random syscall using CPU and VirtIO entropy sources."""
import os
import pathlib
import select
import subprocess
import tempfile
import time

root=pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="avalanche-entropy-") as directory:
    folder=pathlib.Path(directory)
    obj=folder/"random-probe.o"; executable=folder/"random-probe.elf"
    subprocess.run([os.environ.get("CC","gcc"),"-m64","-mno-red-zone","-ffreestanding",
                    "-fno-pie","-fno-stack-protector","-Iavalanche/include","-Iavalanche/user",
                    "-c","avalanche/tools/random-probe.c","-o",str(obj)],cwd=root,check=True)
    subprocess.run([os.environ.get("LD","ld"),"-m","elf_x86_64","-T","avalanche/user/linker.ld",
                    "-nostdlib","-o",str(executable),"avalanche/build/user/crt.o",str(obj),
                    "avalanche/build/user/lib.o"],cwd=root,check=True)
    disk=folder/"data.img"
    subprocess.run(["python3",str(root/"avalanche/tools/mkdisk.py"),str(disk),str(executable)],check=True)
    for cpu,rng,expected in [("qemu64,-rdrand",True,b"HARDWARE RANDOM OK"),
                             ("qemu64,-rdrand",False,b"RANDOM UNAVAILABLE"),
                             ("max",False,b"HARDWARE RANDOM OK")]:
        command=[os.environ.get("QEMU","qemu-system-x86_64"),"-accel","tcg","-cpu",cpu,
                 "-m","256","-boot","d","-vga","none","-display","none","-nic","none",
                 "-monitor","none","-serial","stdio","-no-reboot",
                 "-cdrom",str(root/"avalanche/build/avalanche.iso"),
                 "-drive",f"file={disk},format=raw,if=ide"]
        for variable,option in [("QEMU_BIOS","-bios"),("QEMU_DATA","-L")]:
            if os.environ.get(variable): command.extend([option,os.environ[variable]])
        if rng: command.extend(["-device","virtio-rng-pci,disable-modern=on"])
        process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        def until_prompt():
            output=b""; deadline=time.monotonic()+40
            while b"avalanche> " not in output and time.monotonic()<deadline:
                if select.select([process.stdout],[],[],.5)[0]:
                    data=os.read(process.stdout.fileno(),4096)
                    if not data: break
                    output+=data
            assert b"avalanche> " in output,output
            assert b"KERNEL EXCEPTION" not in output,output
            return output
        try:
            until_prompt()
            process.stdin.write(b"run random-probe\n"); process.stdin.flush()
            output=until_prompt(); assert expected in output,output
            print("PASS "+("VirtIO entropy on a CPU without RDRAND" if rng else
                            "CPU hardware entropy" if cpu=="max" else
                            "missing hardware entropy returns an error"))
        finally:
            if process.poll() is None: process.terminate(); process.wait(timeout=5)
