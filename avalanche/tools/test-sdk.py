#!/usr/bin/env python3
"""Build and sign an app, upload it, verify it in Avalanche, and run it in ring 3."""
import importlib.util
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from types import SimpleNamespace

root=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('storage',pathlib.Path(__file__).with_name('test-storage.py'))
storage=importlib.util.module_from_spec(spec);spec.loader.exec_module(storage)
with tempfile.TemporaryDirectory(prefix='magenta-sdk-') as temporary:
    folder=pathlib.Path(temporary);project=folder/'sample';keys=folder/'keys';signed=folder/'signed'
    tool=root/'sdk/bin/mgnta'
    subprocess.run([str(tool),'init',str(project)],check=True)
    subprocess.run([str(tool),'build',str(project)],check=True)
    subprocess.run([str(tool),'keygen','--out',str(keys),'--identity','SDK integration check'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    subprocess.run([str(root/'sdk/bin/sign'),str(project/'build/sample.mgnta'),'--key',str(keys/'private.pem'),'--certificate',str(keys/'certificate.pem'),'--out',str(signed)],check=True)
    subprocess.run([str(tool),'verify',str(signed/'sample.mgnta.manifest'),'--certificate',str(signed/'trust.der')],check=True)
    print('PASS SDK builds native .mgnta and signs with a matching RSA developer identity',flush=True)
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    uploads=folder/'uploads'
    log=(folder/'uploads.log').open('wb')
    server=subprocess.Popen(['python3',str(root/'tools/package-upload.py'),'--root',str(uploads),'--port',str(port)],stdout=log,stderr=log)
    machine=None
    try:
        url=f'http://127.0.0.1:{port}'
        for i in range(100):
            try:
                with urllib.request.urlopen(url,timeout=1) as response:page=response.read().decode()
                break
            except OSError:time.sleep(.05)
        else:raise RuntimeError('Upload service did not start')
        token=re.search(r'name="token" value="([^"]+)"',page)[1]
        boundary='MagentaSDKCheck'
        data=(f'--{boundary}\r\nContent-Disposition: form-data; name="token"\r\n\r\n{token}\r\n--{boundary}\r\nContent-Disposition: form-data; name="bundle"; filename="sample.mgnta.zip"\r\nContent-Type: application/zip\r\n\r\n').encode()+(signed/'sample.mgnta.zip').read_bytes()+f'\r\n--{boundary}--\r\n'.encode()
        request=urllib.request.Request(url+'/upload',data=data,headers={'Content-Type':'multipart/form-data; boundary='+boundary})
        with urllib.request.urlopen(request,timeout=20) as response:assert b'Verified sample.mgnta' in response.read()
        print('PASS upload service verifies executable digest and developer signature',flush=True)
        image=folder/'ext4.img'
        with image.open('wb') as file:file.truncate(128*1024**2)
        subprocess.run(['mkfs.ext4','-q','-F','-b','4096',str(image)],check=True)
        disk=folder/'data.img';shutil.copyfile(root/'Images/magenta-data.img',disk)
        args=SimpleNamespace(qemu=os.environ.get('QEMU','qemu-system-x86_64'),bios=os.environ.get('QEMU_BIOS'),qemu_data=os.environ.get('QEMU_DATA'),network=True)
        machine=storage.Machine(args,disk,image,folder)
        machine.until(b'Magenta> ');machine.send('0',b'Power> ');machine.send('3',b'to confirm: ');machine.send('RESCUE',b'avalanche> ')
        machine.command('dhcp')
        host=f'http://10.0.2.2:{port}'
        storage.contains(machine.command(f'pkg trust-url {host} /latest/trust.der'),b'Signing certificate SHA-256: ')
        storage.contains(machine.command(f'pkg fetch {host} /latest/app.manifest'),b'Installed sample.mgnta')
        storage.contains(machine.command('run sample.mgnta'),b'Hello from a native Magenta application!')
        print('PASS OS package manager verifies uploaded .mgnta and runs the SDK app in ring 3',flush=True)
        current=json.loads((uploads/'latest.json').read_text());target=uploads/current['id']/current['payload']
        payload=target.read_bytes();target.write_bytes(payload[:-1]+bytes([payload[-1]^1]))
        storage.contains(machine.command(f'pkg fetch {host} /latest/app.manifest'),b'Package download error 74')
        storage.contains(machine.command('run sample.mgnta'),b'Hello from a native Magenta application!')
        print('PASS changed upload is rejected and the installed executable is preserved',flush=True)
        machine.send('run ush',b'user> ')
        for command,expected in [('mkdir /work',b'Directory created.'),('write /work/note hello sdk',b'Saved.'),('cp /work/note /work/copy',b'Done.'),('mv /work/copy /work/moved',b'Done.'),('grep sdk /work/moved',b'hello sdk'),('wc /work/moved',b'2 words 9 bytes'),('uname',b'Avalanche x86_64'),('free',b'Free KiB:')]:
            storage.contains(machine.send(command,b'user> '),expected)
        machine.send('exit',b'avalanche> ')
        print('PASS native Unix-style file tools, grep, wc, uname and memory reporting',flush=True)
        machine.stop();machine=None
        subprocess.run(['e2fsck','-fn',str(image)],check=True,stdout=subprocess.DEVNULL)
        print('PASS signed installation and Unix file operations leave ext4 clean',flush=True)
    finally:
        if machine:machine.stop()
        server.terminate();server.wait(timeout=5);log.close()
