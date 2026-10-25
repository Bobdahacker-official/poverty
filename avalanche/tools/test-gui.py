#!/usr/bin/env python3
import importlib.util
import json
import os
import pathlib
import shutil
import socket
import tempfile
import time
import subprocess
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
print(f'GUI artifacts: {folder}',flush=True)
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
    print('QEMU mice:',qmp('query-mice'),flush=True)
    def screenshot():
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            qmp('screendump',{'filename':str(folder/'desktop.ppm')})
            header,dimensions,maximum,pixels=(folder/'desktop.ppm').read_bytes().split(b'\n',3)
            width,height=map(int,dimensions.split())
            def pixel(x,y): return pixels[(y*width+x)*3:(y*width+x)*3+3]
            if width>=640 and height>=480 and len(pixels)==width*height*3:
                panel=pixel(width//2,height//2)
                dock=pixel(width//2,height-24)
                background=pixel(20,height//2)
                if min(panel)>170 and min(dock)>160 and max(background)<150 and pixels.count(b'\xe6\x00\x7e')>50: return
            time.sleep(.1)
        raise AssertionError('compositor pixels do not match the light settings panel, glass dock and dark wallpaper')
    storage.report('compositor screenshot and pixel checks',screenshot)
    mouse_position=[0,0]
    def click(x,y):
        dx,dy=x-mouse_position[0],y-mouse_position[1]
        while dx or dy:
            step_x=max(-60,min(60,dx));step_y=max(-60,min(60,dy))
            qmp('human-monitor-command',{'command-line':f'mouse_move {step_x} {step_y}'})
            dx-=step_x;dy-=step_y;time.sleep(.04)
        mouse_position[:]=[x,y]
        time.sleep(.2)
        qmp('human-monitor-command',{'command-line':'mouse_button 1'})
        time.sleep(.25)
        qmp('human-monitor-command',{'command-line':'mouse_button 0'})
        time.sleep(.4)
    def dark_panel(filename):
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            qmp('screendump',{'filename':str(filename)})
            header,dimensions,maximum,pixels=filename.read_bytes().split(b'\n',3)
            width,height=map(int,dimensions.split())
            sample=pixels[((height//2)*width+width//2)*3:((height//2)*width+width//2)*3+3]
            if max(sample)<150 and pixels.count(b'\xe6\x00\x7e')>50:return
            time.sleep(.1)
        print('VM status:',qmp('query-status'),flush=True)
        symbols={line.split()[-1]:line.split()[0] for line in subprocess.check_output(['nm','-a',str(storage.ROOT/'avalanche/build/avalanche.elf')],text=True).splitlines() if len(line.split())==3}
        for name in ('mouse_x','mouse_y','mouse_ready','mouse_buttons','packet_n','ticks'):
            if name in symbols:print(name,qmp('human-monitor-command',{'command-line':f'xp /8bx 0x{symbols[name]}'}),flush=True)
        print(qmp('human-monitor-command',{'command-line':'info registers'}),flush=True)
        raise AssertionError('theme selection did not change the settings palette')
    def expanded(expected):
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            qmp('screendump',{'filename':str(folder/'window.ppm')})
            _,dimensions,_,pixels=(folder/'window.ppm').read_bytes().split(b'\n',3)
            width,height=map(int,dimensions.split());sample=pixels[(250*width+30)*3:(250*width+30)*3+3]
            if (min(sample)>160)==expected:return
            time.sleep(.1)
        raise AssertionError('window maximize/restore did not update its bounds')
    click(839,108)
    storage.report('window maximize button',lambda: expanded(True))
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-m'})
    storage.report('window restore shortcut',lambda: expanded(False))
    click(800,300)
    storage.report('mouse-operated appearance theme',lambda: dark_panel(folder/'appearance.ppm'))
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-n'})
    machine.until(b'[Magenta] Network');time.sleep(.2)
    click(910,108)
    storage.report('window close button',lambda: machine.until(b'[Magenta] Window closed'))
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-e'})
    storage.report('graphical editor launch',lambda: machine.until(b'[Magenta] Text editor'))
    time.sleep(.15)
    for character in 'guitest':
        qmp('human-monitor-command',{'command-line':'sendkey '+character})
        time.sleep(.15)
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-s'})
    storage.report('graphical editor save',lambda: machine.until(b'[Magenta] File saved'))
    time.sleep(.15)
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-p'})
    machine.until(b'[Magenta] Packages');time.sleep(.2)
    qmp('screendump',{'filename':str(folder/'packages.ppm')})
    click(700,355)
    qmp('screendump',{'filename':str(folder/'packages-click.ppm')})
    storage.report('package install button',lambda: machine.until(b'[Magenta] Installed hello',10))
    click(780,355)
    qmp('screendump',{'filename':str(folder/'package-remove.ppm')})
    click(420,410)
    qmp('screendump',{'filename':str(folder/'package-confirm.ppm')})
    storage.report('package remove confirmation',lambda: machine.until(b'[Magenta] Package database updated',10))
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-t'})
    storage.report('graphical ring-3 terminal',lambda: machine.until(b'user> '))
    time.sleep(.2)
    for character in 'echo ui-shell-ready':
        key='spc' if character==' ' else 'minus' if character=='-' else character
        qmp('human-monitor-command',{'command-line':'sendkey '+key})
        time.sleep(.15)
    qmp('human-monitor-command',{'command-line':'sendkey ret'})
    storage.report('graphical terminal command execution',lambda: machine.until(b'\r\nui-shell-ready\r\n'))
    time.sleep(.2)
    qmp('human-monitor-command',{'command-line':'sendkey ctrl-e'})
    machine.until(b'[Magenta] Text editor');time.sleep(.2)
    qmp('human-monitor-command',{'command-line':'sendkey x'});time.sleep(.2)
    qmp('human-monitor-command',{'command-line':'sendkey esc'})
    def unsaved_dialog():
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            qmp('screendump',{'filename':str(folder/'unsaved.ppm')})
            _,dimensions,_,pixels=(folder/'unsaved.ppm').read_bytes().split(b'\n',3)
            width,height=map(int,dimensions.split());sample=pixels[(405*width+386)*3:(405*width+386)*3+3]
            if sample[0]>150 and sample[1]<80 and sample[2]>70:return
            time.sleep(.1)
        raise AssertionError('unsaved document confirmation was not displayed')
    storage.report('unsaved document exit protection',unsaved_dialog)
    click(515,410)
    storage.report('graphical to text desktop transition',lambda: machine.until(b'Magenta> '))
    machine.send('0',b'Power> ')
    machine.send('3',b'to confirm: ')
    machine.send('RESCUE',b'avalanche> ')
    storage.report('graphical editor persisted file',lambda: storage.contains(machine.command('cat notes'),b'guitest\r\n'))
    storage.report('graphical package removal persisted',lambda: storage.contains(machine.command('pkg search hello'),b'hello 1.0.0 available'))
    stream.close();connection.close()
finally:
    machine.stop()
reboot=folder/'reboot';reboot.mkdir()
machine=storage.Machine(args,data,extra,reboot)
try:
    machine.until(b'Magenta graphical desktop ready')
    connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);connection.settimeout(10)
    connection.connect(str(reboot/'qmp.sock'));stream=connection.makefile('rwb',buffering=0)
    json.loads(stream.readline());qmp('qmp_capabilities')
    storage.report('appearance survives cold restart',lambda: dark_panel(reboot/'desktop.ppm'))
    stream.close();connection.close()
finally:
    machine.stop()
print(f'GUI artifacts: {folder}',flush=True)
