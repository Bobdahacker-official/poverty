#!/usr/bin/env python3
"""Boot the native desktop, open a PNG, and check rendered image pixels."""
import os
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile
import time
import zlib

root = pathlib.Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument("--ext4",action="store_true")
parser.add_argument("--memory",type=int,default=512)
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix="magenta-image-gui-") as directory:
    folder = pathlib.Path(directory)
    def chunk(kind, data):
        return struct.pack(">I",len(data))+kind+data+struct.pack(">I",zlib.crc32(kind+data))
    raw = bytearray()
    for y in range(160):
        raw.append(0)
        for x in range(256):
            raw.extend(([255,0,0] if x<128 else [0,255,0]) if y<80
                       else ([0,0,255] if x<128 else [255,255,0]))
    fixture = folder / "sample.png.bin"
    fixture.write_bytes(b"\x89PNG\r\n\x1a\n"+
                        chunk(b"IHDR",struct.pack(">IIBBBBB",256,160,8,2,0,0,0))+
                        chunk(b"IDAT",zlib.compress(raw))+chunk(b"IEND",b""))
    disk = folder / "data.img"
    applications = sorted((root/"avalanche/build/user").glob("*.elf"))
    if args.ext4:
        source=folder/"root"; source.mkdir()
        image_bytes=fixture.read_bytes()
        # Exercise a real input larger than the legacy 64 KiB file API.
        (source/"sample.png").write_bytes(image_bytes[:-12]+
                                          chunk(b"tEXt",b"Notes\0"+b"A"*70000)+image_bytes[-12:])
        for application in applications:
            shutil.copyfile(application,source/application.stem)
        with disk.open("wb") as file: file.truncate(64*1024*1024)
        subprocess.run(["mkfs.ext4","-F","-q","-b","4096","-d",str(source),str(disk)],check=True)
    else:
        subprocess.run(["python3",str(root/"avalanche/tools/mkdisk.py"),str(disk),
                        *map(str,applications),str(fixture)],check=True)
    serial = folder / "serial.log"
    screenshot = folder / "viewer.ppm"
    command = [os.environ.get("QEMU","qemu-system-x86_64"),"-accel","tcg",
               "-m",str(args.memory),"-boot","d","-display","none","-vga","std",
               "-monitor","stdio","-serial",f"file:{serial}","-nic","none",
               "-cdrom",str(root/"Images/magenta.iso"),
               "-drive",f"file={disk},format=raw,if=ide"]
    for variable,option in [("QEMU_BIOS","-bios"),("QEMU_DATA","-L")]:
        if os.environ.get(variable): command.extend([option,os.environ[variable]])
    if os.environ.get("QEMU_VGA_ROM"):
        command.extend(["-global","VGA.romfile="+os.environ["QEMU_VGA_ROM"]])
    process = subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            if serial.exists() and b"Magenta graphical desktop ready" in serial.read_bytes(): break
            if process.poll() is not None: raise RuntimeError(process.stderr.read().decode())
            time.sleep(.2)
        else: raise RuntimeError(serial.read_text(errors="replace") if serial.exists() else "No boot output")
        def monitor(text):
            process.stdin.write((text+"\n").encode()); process.stdin.flush()
        time.sleep(1)
        monitor("sendkey ctrl-u")
        time.sleep(1)
        for key in ["slash","s","a","m","p","l","e","dot","p","n","g","ret"]:
            monitor("sendkey "+key); time.sleep(.15)
        time.sleep(2)
        monitor("screendump "+str(screenshot))
        deadline=time.monotonic()+10
        while not screenshot.exists() and time.monotonic()<deadline: time.sleep(.1)
        time.sleep(.3)
        if os.environ.get("GUI_CAPTURE"):
            pathlib.Path(os.environ["GUI_CAPTURE"]).write_bytes(screenshot.read_bytes())
        magic,dimensions,maximum,pixels=screenshot.read_bytes().split(b"\n",3)
        width,height=map(int,dimensions.split())
        assert magic==b"P6" and maximum==b"255" and len(pixels)==width*height*3
        for color in [b"\xff\x00\x00",b"\x00\xff\x00",b"\x00\x00\xff",b"\xff\xff\x00"]:
            assert pixels.count(color)>2000, serial.read_text(errors="replace")[-1200:]
        print("PASS native image viewer opens PNG and renders all four color quadrants"+
              (" from ext4 above 64 KiB" if args.ext4 else " from Avalanche FS"))
    finally:
        if process.poll() is None:
            process.stdin.write(b"quit\n"); process.stdin.flush()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
