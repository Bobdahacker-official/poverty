#!/usr/bin/env python3
import importlib.util
import json
import os
import pathlib
import shutil
import socket
import tempfile
import time
from types import SimpleNamespace

spec=importlib.util.spec_from_file_location('storage',pathlib.Path(__file__).with_name('test-storage.py'))
storage=importlib.util.module_from_spec(spec);spec.loader.exec_module(storage)
folder=pathlib.Path(tempfile.mkdtemp(prefix='magenta-gui-'))
data=folder/'data.img';extra=folder/'extra.img'
shutil.copyfile(storage.ROOT/'magenta/images/magenta-data.img',data)
with extra.open('wb') as file: file.truncate(16*1024**2)
args=SimpleNamespace(qemu=os.environ.get('QEMU','qemu-system-x86_64'),
                     bios=os.environ.get('QEMU_BIOS'),qemu_data=os.environ.get('QEMU_DATA'),
                     graphics=True,vga_rom=os.environ.get('QEMU_VGA_ROM','vgabios-stdvga.bin'))
machine=storage.Machine(args,data,extra,folder)
try:
    storage.report('graphical desktop boot',lambda: machine.until(b'Magenta graphical desktop ready'))
    connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);connection.settimeout(10)
    connection.connect(str(folder/'qmp.sock'));stream=connection.makefile('rwb',buffering=0)
    json.loads(stream.readline())
    def qmp(command,arguments=None):
        message={'execute':command}
        if arguments: message['arguments']=arguments
        stream.write(json.dumps(message).encode()+b'\n')
        while True:
            response=json.loads(stream.readline())
            if 'error' in response: raise RuntimeError(response['error'])
            if 'return' in response: return response['return']
    qmp('qmp_capabilities')
    def screenshot():
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            qmp('screendump',{'filename':str(folder/'desktop.ppm')})
            header,dimensions,maximum,pixels=(folder/'desktop.ppm').read_bytes().split(b'\n',3)
            width,height=map(int,dimensions.split())
            def pixel(x,y): return pixels[(y*width+x)*3:(y*width+x)*3+3]
            if width>=640 and height>=480 and len(pixels)==width*height*3:
                r,g,b=pixel(450,95)
                bar=pixel(60,height-44);background=pixel(width-10,height-80)
                if r>170 and r>g+50 and b>g+20 and bar[0]>bar[1]+50 and background[0]>background[1] and background[2]>background[0]: return
            time.sleep(.1)
        raise AssertionError('compositor pixels do not match the magenta title and background gradient')
    storage.report('compositor screenshot and pixel checks',screenshot)
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-e'})
    storage.report('graphical editor launch',lambda: machine.until(b'[Magenta] Text editor'))
    time.sleep(.15)
    for character in 'guitest':
        qmp('human-monitor-command',{'command-line':'sendkey '+character})
        time.sleep(.15)
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-s'})
    storage.report('graphical editor save',lambda: machine.until(b'[Magenta] File saved'))
    time.sleep(.15)
    qmp('human-monitor-command',{'command-line':'sendkey esc'})
    storage.report('graphical to text desktop transition',lambda: machine.until(b'Magenta> '))
    machine.send('0',b'Power> ')
    machine.send('3',b'to confirm: ')
    machine.send('RESCUE',b'avalanche> ')
    storage.report('graphical editor persisted file',lambda: storage.contains(machine.command('cat notes'),b'guitest'))
    stream.close();connection.close()
finally:
    machine.stop()
print(f'GUI artifacts: {folder}',flush=True)
