#!/usr/bin/env python3
"""Check native desktop controls using rendered labels and current window bounds."""
import importlib.util
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from types import SimpleNamespace

spec=importlib.util.spec_from_file_location('storage',pathlib.Path(__file__).with_name('test-storage.py'))
storage=importlib.util.module_from_spec(spec);spec.loader.exec_module(storage)
folder=pathlib.Path(tempfile.mkdtemp(prefix='magenta-gui-'))
data=folder/'data.img';extra=folder/'extra.img'
shutil.copyfile(storage.ROOT/'Images/magenta-data.img',data)
with extra.open('wb') as file: file.truncate(16*1024**2)
helper=folder/'gui-layout'
subprocess.run([os.environ.get('CC','gcc'),'-O2','-flto','-ffunction-sections',
                '-fdata-sections','-Wl,--gc-sections','-Iavalanche/include',
                'avalanche/tools/gui-test-layout.c','-o',str(helper)],cwd=storage.ROOT,check=True)
layout=json.loads(subprocess.check_output([str(helper)]))
symbols={}
for line in subprocess.check_output(['nm','-S',str(storage.ROOT/'avalanche/build/avalanche.elf')],text=True).splitlines():
    fields=line.split()
    if len(fields)==4: symbols[fields[3]]=(int(fields[0],16),int(fields[1],16))
args=SimpleNamespace(qemu=os.environ.get('QEMU','qemu-system-x86_64'),
                     bios=os.environ.get('QEMU_BIOS'),qemu_data=os.environ.get('QEMU_DATA'),
                     graphics=True,absolute_pointer=True,vga_rom=os.environ.get('QEMU_VGA_ROM','vgabios-stdvga.bin'))
machine=storage.Machine(args,data,extra,folder)
print(f'GUI artifacts: {folder}',flush=True)
connection=None
try:
    storage.report('graphical desktop boot',lambda: machine.until(b'Magenta graphical desktop ready'))
    connection=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);connection.settimeout(15)
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
    def monitor(command): return qmp('human-monitor-command',{'command-line':command})
    mice=qmp('query-mice')
    assert any(device['current'] and device['absolute'] for device in mice),mice
    print('PASS absolute virtual pointer detection',flush=True)
    def memory(address,size):
        path=folder/'memory.bin'
        result=monitor(f'pmemsave {address:#x} {size} "{path}"')
        assert path.exists(),result
        return path.read_bytes()
    def value(name,format='I',offset=0):
        return struct.unpack(format,memory(symbols[name][0]+offset,struct.calcsize(format)))[0]
    scale=value('native_scale','f')
    def state(app):
        raw=memory(symbols['apps'][0]+app*layout['app_size'],layout['app_size'])
        return {'visible':bool(raw[layout['app_visible']]),
                'maximized':bool(raw[layout['app_maximized']]),
                'bounds':struct.unpack_from('<4f',raw,layout['app_bounds'])}
    def capture(name):
        path=folder/(name+'.ppm');qmp('screendump',{'filename':str(path)})
        magic,dimensions,maximum,pixels=path.read_bytes().split(b'\n',3)
        width,height=map(int,dimensions.split())
        assert magic==b'P6' and maximum==b'255' and len(pixels)==width*height*3
        return width,height,pixels
    time.sleep(1)
    width,height,desktop=capture('desktop')
    assert width>=640 and height>=480
    assert len(set(desktop))>64 and desktop.count(b'\xe6\x00\x7e')>20
    assert not any(state(app)['visible'] for app in range(layout['app_count']))
    print('PASS desktop pixels and clean startup without Settings',flush=True)
    def key(text,delay=.25): monitor('sendkey '+text);time.sleep(delay)
    def move(x,y):
        px=max(0,min(width-1,x*scale));py=max(0,min(height-1,y*scale))
        qmp('input-send-event',{'events':[
            {'type':'abs','data':{'axis':'x','value':round(px*32767/(width-1))}},
            {'type':'abs','data':{'axis':'y','value':round(py*32767/(height-1))}}]})
        time.sleep(.15)
    def button(down):
        qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        time.sleep(.18)
    def click(x,y): move(x,y);button(True);button(False);time.sleep(.25)
    def wait(check,message,timeout=12):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            if check(): print('PASS '+message,flush=True);return
            time.sleep(.15)
        raise AssertionError(message)
    def open_app(shortcut,app):
        key(shortcut)
        wait(lambda:state(app)['visible'],'open application '+str(app))
        time.sleep(.7)
    def control(app,index):
        x,y,w,h=state(app)['bounds'];click(x+w-144+44*index+20,y+28)
    def labels():
        raw=memory(symbols['previous_commands'][0],value('previous_size','Q'))
        result=[];start=layout['text_string']
        for at in range(0,len(raw)-start,8):
            if struct.unpack_from('<I',raw,at)[0]!=layout['text_type']: continue
            length=struct.unpack_from('<i',raw,at+layout['text_length'])[0]
            if length<=0 or length>2048 or at+start+length>len(raw): continue
            text=raw[at+start:at+start+length].decode(errors='replace')
            x=struct.unpack_from('<h',raw,at+layout['text_x'])[0]
            y=struct.unpack_from('<h',raw,at+layout['text_y'])[0]
            w=struct.unpack_from('<H',raw,at+layout['text_w'])[0]
            h=struct.unpack_from('<H',raw,at+layout['text_h'])[0]
            result.append((text,x,y,w,h))
        return result
    def click_label(text,last=False):
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            matches=[item for item in labels() if item[0]==text and item[3]>0 and item[4]>0]
            if matches:
                _,x,y,w,h=matches[-1 if last else 0];click(x+w/2,y+h/2);return
            time.sleep(.2)
        raise AssertionError('No rendered control labelled '+text)
    def region(pixels,x,y,w=32,h=32):
        return b''.join(pixels[((y+row)*width+x)*3:((y+row)*width+x+w)*3] for row in range(h))
    pointer_y=height*.55/scale
    move(8,pointer_y);time.sleep(.3)
    move(48,pointer_y);time.sleep(.3)
    restored=capture('cursor')[2]
    px=round(8*scale);py=round(pointer_y*scale)
    assert region(restored,px,py)==region(desktop,px,py)
    assert abs(value('mouse_x','i')-round(48*scale))<=2
    print('PASS cursor movement and background restoration',flush=True)
    settings=layout['settings'];network=layout['network'];editor=layout['editor']
    packages=layout['packages'];terminal=layout['terminal']
    open_app('ctrl-o',settings)
    click_label('Latte')
    wait(lambda:value('preferences',offset=layout['theme_offset'])==0,'mouse appearance theme selection')
    capture('light-settings')
    click_label('Mocha')
    wait(lambda:value('preferences',offset=layout['theme_offset'])==3,'dark appearance theme selection')
    x,y,w,h=state(settings)['bounds']
    move(x+160,y+26);button(True);move(x+172,y+38);button(False)
    wait(lambda:abs(state(settings)['bounds'][0]-x)>2,'window dragging')
    x,y,w,h=state(settings)['bounds']
    move(x+w-5,y+h-5);button(True);move(x+w+11,y+h+5);button(False)
    wait(lambda:state(settings)['bounds'][2]>w+8,'window resizing')
    before=state(settings)['bounds']
    control(settings,1)
    wait(lambda:state(settings)['maximized'],'window maximize button')
    key('ctrl-m')
    wait(lambda:not state(settings)['maximized'],'window restore shortcut')
    assert all(abs(a-b)<2 for a,b in zip(before,state(settings)['bounds']))
    control(settings,0);time.sleep(.5)
    minimized=capture('minimized')[2]
    open_app('ctrl-o',settings)
    restored=capture('restored')[2]
    assert minimized!=restored
    print('PASS window minimize and restore',flush=True)
    control(settings,2)
    wait(lambda:not state(settings)['visible'],'window close button')
    open_app('ctrl-n',network)
    control(network,2)
    wait(lambda:not state(network)['visible'],'second application close button')
    open_app('ctrl-e',editor)
    for character in 'guitest': key(character,.15)
    key('ctrl-s');machine.until(b'[Magenta] File saved')
    print('PASS graphical editor save',flush=True)
    open_app('ctrl-p',packages)
    click_label('hello');click_label('Install');machine.until(b'[Magenta] Installed hello')
    print('PASS package install button',flush=True)
    click_label('Remove')
    wait(lambda:bool(value('removing_package','B')),'package removal confirmation opens')
    click_label('Confirm')
    machine.until(b'[Magenta] Package database updated')
    print('PASS package remove confirmation',flush=True)
    open_app('ctrl-t',terminal);machine.until(b'user> ')
    for character in 'echo ui-shell-ready':
        key('spc' if character==' ' else 'minus' if character=='-' else character,.15)
    key('ret');machine.until(b'\r\nui-shell-ready\r\n')
    print('PASS graphical ring-3 terminal command execution',flush=True)
    open_app('ctrl-e',editor);key('x');key('esc')
    wait(lambda:bool(value('editor_discard','B')),'unsaved document exit protection')
    click_label('Discard',last=True);machine.until(b'Magenta> ')
    print('PASS graphical to text desktop transition',flush=True)
    machine.send('0',b'Power> ');machine.send('3',b'to confirm: ');machine.send('RESCUE',b'avalanche> ')
    storage.report('graphical editor persisted file',lambda:storage.contains(machine.command('cat notes'),b'guitest'))
    storage.report('graphical package removal persisted',lambda:storage.contains(machine.command('pkg search hello'),b'hello 1.0.0 available'))
finally:
    if connection: connection.close()
    machine.stop()
