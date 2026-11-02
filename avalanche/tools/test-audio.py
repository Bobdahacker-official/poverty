#!/usr/bin/env python3
"""Play a streamed WAV through HDA and inspect captured analogue PCM."""
import math
import argparse
import json
import os
import pathlib
import select
import socket
import struct
import subprocess
import tempfile
import time
import wave

root=pathlib.Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser();parser.add_argument("--gui",action="store_true")
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix="avalanche-audio-") as directory:
    folder=pathlib.Path(directory);source=folder/"root";source.mkdir()
    with wave.open(str(source/"tone.wav"),"wb") as wav:
        wav.setparams((2,2,48000,0,"NONE","not compressed"))
        wav.writeframes(b"".join(struct.pack("<hh",int(12000*math.sin(2*math.pi*440*i/48000)),
                                            int(9000*math.sin(2*math.pi*660*i/48000)))
                                 for i in range(48000)))
    recordings=source/"recordings-for-native-audio"
    recordings.mkdir()
    (recordings/"tone.wav").write_bytes((source/"tone.wav").read_bytes())
    disk=folder/"data.img";capture=folder/"captured.wav"
    with disk.open("wb") as file:file.truncate(64*1024**2)
    subprocess.run(["mkfs.ext4","-q","-F","-b","4096","-d",str(source),str(disk)],check=True)
    command=[os.environ.get("QEMU","qemu-system-x86_64"),"-accel","tcg","-m","256",
             "-boot","d","-vga","none","-display","none","-monitor","none","-serial","stdio",
             "-nic","none","-no-reboot","-cdrom",str(root/"avalanche/build/avalanche.iso"),
             "-drive",f"file={disk},format=raw,if=ide",
             "-audiodev",f"wav,id=audio,path={capture},out.frequency=48000",
             "-device","intel-hda","-device","hda-duplex,audiodev=audio"]
    if args.gui:
        command[command.index("-vga")+1]="std"
        command += ["-qmp",f"unix:{folder}/qmp.sock,server=on,wait=off"]
        if os.environ.get("QEMU_VGA_ROM"):
            command += ["-global","VGA.romfile="+os.environ["QEMU_VGA_ROM"]]
    for variable,option in [("QEMU_BIOS","-bios"),("QEMU_DATA","-L")]:
        if os.environ.get(variable):command.extend([option,os.environ[variable]])
    machine=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    pending=bytearray()
    def until(needle,timeout=60):
        global pending
        deadline=time.monotonic()+timeout
        while needle not in pending and time.monotonic()<deadline:
            if select.select([machine.stdout],[],[],.2)[0]:
                data=os.read(machine.stdout.fileno(),65536)
                if not data:break
                pending.extend(data)
        assert needle in pending,pending.decode(errors="replace")
        end=pending.index(needle)+len(needle)
        output=bytes(pending[:end]);del pending[:end]
        assert b"KERNEL EXCEPTION" not in output,output
        return output
    def prompt():return until(b"avalanche> ")
    def run(text):
        machine.stdin.write((text+"\n").encode());machine.stdin.flush();return prompt()
    connection=None
    try:
        boot=prompt();assert b"Intel HD Audio PCM output ready" in boot,boot
        info=run("audio");assert b"PCM16 output" in info,info
        if args.gui:
            machine.stdin.write(b"gui\n");machine.stdin.flush()
            until(b"Magenta graphical desktop ready")
            connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
            connection.settimeout(15);connection.connect(str(folder/"qmp.sock"))
            stream=connection.makefile("rwb",buffering=0);stream.readline()
            def qmp(name,arguments=None):
                message={"execute":name}
                if arguments:message["arguments"]=arguments
                stream.write(json.dumps(message).encode()+b"\n")
                while True:
                    response=json.loads(stream.readline());assert "error" not in response,response
                    if "return" in response:return response["return"]
            qmp("qmp_capabilities")
            def key(text,delay=.12):
                qmp("human-monitor-command",{"command-line":"sendkey "+text+" 50"});time.sleep(delay)
            key("ctrl-d",.5)
            until(b"[Magenta] Music")
            for text in ["slash","t","o","n","e","dot","w","a","v"]:key(text)
            key("ret",.15)
            started=time.monotonic();key("ctrl-f",0)
            until(b"[Magenta] Files",.75)
            assert time.monotonic()-started<.75
            time.sleep(1.5)
            key("ctrl-d",0)
            until(b"[Magenta] Music")
            time.sleep(1)
            screenshot=folder/"music.ppm"
            qmp("screendump",{"filename":str(screenshot)})
            pixels=screenshot.read_bytes().split(b"\n",3)[3]
            assert pixels.count(b"\xe6\x00\x7e")>20
            if os.environ.get("AUDIO_GUI_CAPTURE"):
                pathlib.Path(os.environ["AUDIO_GUI_CAPTURE"]).write_bytes(screenshot.read_bytes())
            print("PASS Music window plays WAV while Files opens without blocking",flush=True)
        else:
            output=run("run aplay /recordings-for-native-audio/tone.wav")
            assert b"Playback complete." in output and b"Process exit: 0" in output,output
        print("PASS HDA codec route and streamed WAV playback above 64 KiB",flush=True)
        time.sleep(.3)
    finally:
        if connection:connection.close()
        if machine.poll() is None:machine.terminate();machine.wait(timeout=5)
    with wave.open(str(capture),"rb") as wav:
        assert wav.getnchannels()==2 and wav.getsampwidth()==2
        data=wav.readframes(wav.getnframes())
        samples=struct.unpack("<"+"h"*(len(data)//2),data)
        left=samples[::2];right=samples[1::2]
        assert max(left)>5000 and min(left)<-5000 and max(right)>4000 and min(right)<-4000
        def crossings(channel):
            return sum(a<=0<b for a,b in zip(channel,channel[1:]))
        a,b=crossings(left),crossings(right)
        assert 420<=a<=460 and 630<=b<=690,(a,b,len(left))
    print("PASS captured HDA stereo samples contain the expected 440/660 Hz channels",flush=True)
