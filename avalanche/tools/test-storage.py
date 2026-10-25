#!/usr/bin/env python3
import argparse
import os
import pathlib
import selectors
import shutil
import subprocess
import tempfile
import time
import struct
import hashlib
import http.server
import threading
import functools
import re

server=None

ROOT = pathlib.Path(__file__).resolve().parents[2]

def run(command):
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors='replace'))
    return result.stdout

def report(name, action):
    try:
        action()
    except Exception as error:
        print(f'FAIL {name}: {error}', flush=True)
        raise
    print(f'PASS {name}', flush=True)

class Machine:
    def __init__(self, args, data, ext4, folder):
        command = [args.qemu, '-accel', 'tcg', '-cpu', 'qemu64', '-m', '256',
                   '-display', 'none', '-vga', 'none', '-serial', 'stdio',
                   '-monitor', 'none', '-no-reboot', '-no-shutdown', '-boot', 'd',
                   '-cdrom', str(ROOT/'Images/magenta.iso'),
                   '-drive', f'file={data},format=raw,if=ide',
                   '-drive', f'file={ext4},format=raw,if=ide', '-nic', 'none']
        if args.bios:
            command += ['-bios', args.bios]
        if args.qemu_data:
            command += ['-L', args.qemu_data]
        if getattr(args,'network',False):
            index=command.index('-nic');del command[index:index+2]
            command+=['-netdev','user,id=net','-device','e1000,netdev=net,romfile=']
        if getattr(args,'graphics',False):
            index=command.index('-vga'); del command[index:index+2]
            command+=['-machine','pc,vmport='+('on' if getattr(args,'absolute_pointer',False) else 'off'),
                      '-device',f'VGA,romfile={args.vga_rom}',
                      '-qmp',f'unix:{folder}/qmp.sock,server=on,wait=off']
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.pending = b''
        self.log = bytearray()
        self.folder = folder
        self.logfile = folder / f'serial-{len(list(folder.glob("serial-*.log")))}.log'

    def until(self, needle, timeout=45):
        deadline = time.monotonic()+timeout
        while needle not in self.pending:
            if time.monotonic() >= deadline:
                raise RuntimeError(f'timed out waiting for {needle!r}: {self.pending[-2000:]!r}')
            for key, _ in self.selector.select(.1):
                data = os.read(key.fd, 65536)
                if not data:
                    raise RuntimeError('QEMU exited unexpectedly')
                self.pending += data
                self.log.extend(data)
        end = self.pending.index(needle)+len(needle)
        output, self.pending = self.pending[:end], self.pending[end:]
        return output

    def send(self, text, prompt):
        self.process.stdin.write((text+'\n').encode())
        self.process.stdin.flush()
        return self.until(prompt)

    def command(self, text):
        return self.send(text, b'avalanche> ')

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.selector.close()
        self.logfile.write_bytes(self.log)

def contains(output, expected):
    if expected not in output:
        raise AssertionError(f'missing {expected!r}: {output[-1500:]!r}')

def inject_transaction(image, committed):
    def physical(path):
        output=run(['debugfs','-R',f'bmap {path} 0',str(image)])
        return int(output.splitlines()[-1])
    journal=physical('<8>')
    target=physical('/sample-text')
    with image.open('r+b') as file:
        file.seek(journal*4096)
        super=bytearray(file.read(4096))
        sequence=struct.unpack_from('>I',super,24)[0]
        start=struct.unpack_from('>I',super,20)[0]
        struct.pack_into('>I',super,28,start)
        file.seek(journal*4096); file.write(super)
        descriptor=bytearray(4096)
        struct.pack_into('>IIIII',descriptor,0,0xc03b3998,1,sequence,target,10)
        payload=bytearray(4096); marker=b'JOURNAL-REPLAY!\n'; payload[:len(marker)]=marker
        commit=bytearray(4096)
        if committed: struct.pack_into('>III',commit,0,0xc03b3998,2,sequence)
        file.seek((journal+start)*4096)
        file.write(descriptor); file.write(payload); file.write(commit)
        file.seek(1024); fs=bytearray(file.read(1024)); fs[96]|=4
        crc=0xffffffff
        for value in fs[:1020]:
            crc^=value
            for _ in range(8): crc=(crc>>1)^(0x82f63b78 if crc&1 else 0)
        struct.pack_into('<I',fs,1020,crc)
        file.seek(1024); file.write(fs); file.flush(); os.fsync(file.fileno())

def main():
    global server
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', default=os.environ.get('QEMU', 'qemu-system-x86_64'))
    parser.add_argument('--bios', default=os.environ.get('QEMU_BIOS'))
    parser.add_argument('--qemu-data', default=os.environ.get('QEMU_DATA'))
    args = parser.parse_args()
    args.network=True
    folder = pathlib.Path(tempfile.mkdtemp(prefix='avalanche-storage-'))
    print(f'Artifacts: {folder}', flush=True)
    repository=folder/'repository';repository.mkdir()
    payload=(ROOT/'avalanche/build/user/hello.elf').read_bytes()
    (repository/'webhello').write_bytes(payload)
    (repository/'badhello').write_bytes(payload)
    for name,digest in [('webhello',hashlib.sha256(payload).hexdigest()),('badhello','0'*64)]:
        (repository/f'{name}.manifest').write_text(f'name {name}\nversion 1.0.0\ndepends hello\nfile {name}\nsha256 {digest}\n')
    handler=functools.partial(http.server.SimpleHTTPRequestHandler,directory=str(repository))
    server=http.server.ThreadingHTTPServer(('127.0.0.1',80),handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    image, source = folder/'ext4.img', folder/'fixture'
    source.mkdir()
    (source/'sample-text').write_bytes(b'EXT4-READ-SUCCESS\n')
    (source/'directory').mkdir()
    (source/'directory'/'nested').write_bytes(b'EXT4-NESTED-SUCCESS\n')
    for index in range(800):
        (source/'directory'/f'entry-{index:04d}').write_bytes(f'ENTRY-{index}\n'.encode())
    with (source/'large').open('wb') as file:
        file.write(b'EXT4-LARGE-START\n')
        for index in range(1, 7):
            file.seek(index*64*1024**2)
            file.write(f'EXTENT-{index}\n'.encode())
        file.seek(5*1024**3)
        file.write(b'EXT4-LARGE-END\n')
    with image.open('wb') as file:
        file.truncate(9*1024**3)
    report('ext4 image creation', lambda: run([
        'mkfs.ext4', '-F', '-b', '4096', '-O', 'extent,dir_index,64bit,flex_bg,has_journal',
        '-E', 'lazy_itable_init=0,lazy_journal_init=0', '-d', str(source), str(image)]))
    def index_directories():
        result = subprocess.run(['e2fsck', '-fyD', str(image)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode not in (0, 1):
            raise RuntimeError(result.stdout.decode(errors='replace'))
    report('host htree directory creation', index_directories)
    report('host e2fsck clean', lambda: run(['e2fsck', '-fn', str(image)]))
    report('host debugfs large file', lambda: contains(
        run(['debugfs', '-R', 'stat /large', str(image)]), str(5*1024**3+len(b'EXT4-LARGE-END\n')).encode()))
    data = folder/'data.img'
    shutil.copyfile(ROOT/'Images/magenta-data.img', data)
    machine = Machine(args, data, image, folder)
    try:
        report('ring-3 Magenta boot', lambda: machine.until(b'Magenta> '))
        machine.send('0', b'Power> ')
        machine.send('3', b'to confirm: ')
        machine.send('RESCUE', b'avalanche> ')
        report('ext4 root directory', lambda: contains(machine.command('ext4ls 1 /'), b'directory'))
        report('ext4 nested lookup', lambda: contains(machine.command('ext4cat 1 /directory/nested'), b'EXT4-NESTED-SUCCESS'))
        report('ext4 htree directory lookup', lambda: contains(machine.command('ext4cat 1 /directory/entry-0799'), b'ENTRY-799'))
        report('ext4 indexed extent traversal', lambda: contains(machine.command('ext4read 1 /large 402653184 9'), b'EXTENT-6'))
        report('ext4 small file', lambda: contains(machine.command('ext4cat 1 /sample-text'), b'EXT4-READ-SUCCESS'))
        machine.command('ext4create 1 /created')
        machine.command('ext4write 1 /created 0 CREATED-IN-GUEST')
        report('ext4 inode and block allocation',lambda: contains(machine.command('ext4cat 1 /created'),b'CREATED-IN-GUEST'))
        machine.command('ext4write 1 /created 5368709120 GROWN-BEYOND-4G')
        report('ext4 file growth beyond 4 GiB',lambda: contains(machine.command('ext4read 1 /created 5368709120 15'),b'GROWN-BEYOND-4G'))
        machine.command('ext4create 1 /directory/created')
        machine.command('ext4write 1 /directory/created 0 CREATED-IN-INDEXED-DIRECTORY')
        report('ext4 indexed directory insertion',lambda: contains(machine.command('ext4cat 1 /directory/created'),b'CREATED-IN-INDEXED-DIRECTORY'))
        report('ext4 journaled overwrite', lambda: contains(machine.command('ext4write 1 /sample-text 0 EXT4-WRITE-OK'), b'avalanche> '))
        report('ext4 write readback', lambda: contains(machine.command('ext4cat 1 /sample-text'), b'EXT4-WRITE-OK'))
        report('ext4 file beyond 4 GiB', lambda: contains(machine.command('ext4read 1 /large 5368709120 15'), b'EXT4-LARGE-END'))
        report('ext4 sparse hole', lambda: contains(machine.command('ext4read 1 /large 4294967296 16'), b'\x00'*16))
        report('cached persistent write', lambda: contains(machine.command('write cache-check CACHE-WRITEBACK-SUCCESS'), b'avalanche> '))
        report('cached read', lambda: contains(machine.command('cat cache-check'), b'CACHE-WRITEBACK-SUCCESS'))
        report('package dependency installation', lambda: contains(machine.command('pkg install http'), b'Installed http'))
        report('package manifest SHA-256', lambda: contains(machine.command('pkg query http'), b'sha256 '))
        report('package database stored on ext4',lambda: contains(machine.command('ext4cat 1 /pkg-http'),b'name http'))
        report('package dependency removal guard', lambda: contains(machine.command('pkg remove udp'), b'installed dependents'))
        report('package search', lambda: contains(machine.command('pkg search http'), b'http 1.0.0 installed'))
        report('DHCP lease',lambda: contains(machine.command('dhcp'),b'avalanche> '))
        report('HTTP package download',lambda: contains(machine.command('pkg fetch 10.0.2.2 /webhello.manifest'),b'Installed webhello'))
        report('download SHA-256 rejection',lambda: contains(machine.command('pkg fetch 10.0.2.2 /badhello.manifest'),b'Package download error 74'))
        report('downloaded package database',lambda: contains(machine.command('pkg query webhello'),b'name webhello'))
        report('downloaded ELF execution',lambda: contains(machine.command('run webhello'),b'Hello from ring 3!'))
        report('Linux anonymous demand paging and TLS',lambda: contains(machine.command('run linuxhello'),b'file, memory, TLS, uname OK'))
        report('copy-on-write fork and wait isolation',lambda: contains(machine.command('run linuxfork'),b'address isolation OK'))
        pids=[]
        def concurrency():
            for _ in range(64):
                output=machine.command('spawn worker sleep')
                match=re.search(rb'Started PID (\d+)',output)
                if not match: raise AssertionError(output)
                pids.append(int(match.group(1)))
            output=machine.command('ps')
            if len(re.findall(rb'\bworker\b',output))<64: raise AssertionError(output)
        report('64 concurrent processes',concurrency)
        report('process priority change',lambda: contains(machine.command(f'priority {pids[0]} 0'),b'avalanche> '))
        report('invalid process priority rejection',lambda: contains(machine.command(f'priority {pids[0]} 32'),b'Operation failed: 22'))
        for pid in pids: machine.command(f'kill {pid}')
        for pid in pids: machine.command(f'wait {pid}')
    finally:
        machine.stop()
    machine = Machine(args, data, image, folder)
    try:
        machine.until(b'Magenta> ')
        machine.send('0', b'Power> ')
        machine.send('3', b'to confirm: ')
        machine.send('RESCUE', b'avalanche> ')
        report('writeback survives cold restart', lambda: contains(machine.command('cat cache-check'), b'CACHE-WRITEBACK-SUCCESS'))
        report('ext4 write survives cold restart', lambda: contains(machine.command('ext4cat 1 /sample-text'), b'EXT4-WRITE-OK'))
        report('ext4 new inode survives restart',lambda: contains(machine.command('ext4read 1 /created 5368709120 15'),b'GROWN-BEYOND-4G'))
        report('package database survives cold restart', lambda: contains(machine.command('pkg list'), b'http 1.0.0 installed'))
        machine.command('pkg remove http')
        report('package removal', lambda: contains(machine.command('pkg search http'), b'http 1.0.0 available'))
    finally:
        machine.stop()
    report('host e2fsck after guest reads', lambda: run(['e2fsck', '-fn', str(image)]))
    for committed in (True, False):
        inject_transaction(image,committed)
        machine=Machine(args,data,image,folder)
        try:
            machine.until(b'Magenta> ')
            machine.send('0',b'Power> ')
            machine.send('3',b'to confirm: ')
            machine.send('RESCUE',b'avalanche> ')
            expected=b'JOURNAL-REPLAY!' if committed else b'EXT4-WRITE-OK'
            report('committed journal replay' if committed else 'incomplete journal discarded',
                   lambda: contains(machine.command('ext4cat 1 /sample-text'),expected))
            machine.command('ext4write 1 /sample-text 0 EXT4-WRITE-OK')
        finally:
            machine.stop()
        report('host e2fsck after replay',lambda: run(['e2fsck','-fn',str(image)]))

if __name__ == '__main__':
    try:
        main()
    finally:
        if server:
            server.shutdown();server.server_close()
