#!/usr/bin/env python3
"""Check hardware-backed user stack NX and read-only ELF text in QEMU."""
import os
import pathlib
import select
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="avalanche-memory-") as directory:
    folder=pathlib.Path(directory)
    for name,flags in [("nxprobe",[]),("rxprobe",["-DPROBE_WRITE"]),
                       ("kernelprobe",["-DPROBE_KERNEL"])]:
        obj=folder/(name+".o")
        subprocess.run([os.environ.get("CC","gcc"),"-m64","-ffreestanding","-fno-pie",
                        *flags,"-c",str(root/"avalanche/tools/nx-probe.S"),"-o",str(obj)],check=True)
        subprocess.run([os.environ.get("LD","ld"),"-m","elf_x86_64","-T",
                        str(root/"avalanche/user/linker.ld"),"-nostdlib","-o",
                        str(folder/(name+".elf")),str(obj)],check=True)
    disk=folder/"data.img"
    subprocess.run(["python3",str(root/"avalanche/tools/mkdisk.py"),str(disk),
                    str(folder/"nxprobe.elf"),str(folder/"rxprobe.elf"),
                    str(folder/"kernelprobe.elf")],check=True)
    for cpu,stack_fault in [("qemu64,+nx",True),("qemu64,-nx",False)]:
        command=[os.environ.get("QEMU","qemu-system-x86_64"),"-accel","tcg",
                 "-cpu",cpu,"-m","256","-boot","d","-vga","none","-display","none",
                 "-monitor","none","-serial","stdio","-nic","none","-no-reboot",
                 "-cdrom",str(root/"avalanche/build/avalanche.iso"),
                 "-drive",f"file={disk},format=raw,if=ide"]
        for variable,option in [("QEMU_BIOS","-bios"),("QEMU_DATA","-L")]:
            if os.environ.get(variable): command.extend([option,os.environ[variable]])
        process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        def until_prompt():
            output=b""; deadline=time.monotonic()+40
            while b"avalanche> " not in output and time.monotonic()<deadline:
                if select.select([process.stdout],[],[],.5)[0]:
                    chunk=os.read(process.stdout.fileno(),4096)
                    if not chunk: break
                    output+=chunk
            assert b"avalanche> " in output,output.decode(errors="replace")
            assert b"KERNEL EXCEPTION" not in output,output
            return output
        try:
            until_prompt()
            process.stdin.write(b"run nxprobe\n"); process.stdin.flush()
            output=until_prompt()
            assert (b"fault 14" in output)==stack_fault,output
            assert (b"Process exit: 139" if stack_fault else b"Process exit: 77") in output,output
            print("PASS user stack NX" if stack_fault else "PASS older CPU without NX boots and runs")
            process.stdin.write(b"run rxprobe\n"); process.stdin.flush()
            output=until_prompt(); assert b"fault 14" in output,output
            print("PASS read-only ELF text rejects user writes")
            process.stdin.write(b"run kernelprobe\n"); process.stdin.flush()
            output=until_prompt(); assert b"fault 14" in output,output
            print("PASS kernel direct memory mapping rejects ring-3 reads")
        finally:
            if process.poll() is None: process.terminate(); process.wait(timeout=5)
