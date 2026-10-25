#!/usr/bin/env python3
import argparse
import os
import pathlib
import selectors
import shutil
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]

def run(command):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT).stdout

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
                   '-cdrom', str(ROOT/'magenta/images/magenta.iso'),
                   '-drive', f'file={data},format=raw,if=ide',
                   '-drive', f'file={ext4},format=raw,if=ide', '-nic', 'none']
        if args.bios:
            command += ['-bios', args.bios]
        if args.qemu_data:
            command += ['-L', args.qemu_data]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.pending = b''
        self.log = bytearray()
        self.folder = folder
        self.logfile = folder / f'serial-{len(list(folder.glob("serial-*.log")))}.log'

    def until(self, needle):
        deadline = time.monotonic()+45
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

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', default=os.environ.get('QEMU', 'qemu-system-x86_64'))
    parser.add_argument('--bios', default=os.environ.get('QEMU_BIOS'))
    parser.add_argument('--qemu-data', default=os.environ.get('QEMU_DATA'))
    args = parser.parse_args()
    folder = pathlib.Path(tempfile.mkdtemp(prefix='avalanche-storage-'))
    print(f'Artifacts: {folder}', flush=True)
    image, source = folder/'ext4.img', folder/'fixture'
    source.mkdir()
    (source/'hello').write_bytes(b'EXT4-READ-SUCCESS\n')
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
    shutil.copyfile(ROOT/'magenta/images/magenta-data.img', data)
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
        report('ext4 small file', lambda: contains(machine.command('ext4cat 1 /hello'), b'EXT4-READ-SUCCESS'))
        report('ext4 file beyond 4 GiB', lambda: contains(machine.command('ext4read 1 /large 5368709120 15'), b'EXT4-LARGE-END'))
        report('ext4 sparse hole', lambda: contains(machine.command('ext4read 1 /large 4294967296 16'), b'\x00'*16))
        report('cached persistent write', lambda: contains(machine.command('write cache-check CACHE-WRITEBACK-SUCCESS'), b'avalanche> '))
        report('cached read', lambda: contains(machine.command('cat cache-check'), b'CACHE-WRITEBACK-SUCCESS'))
    finally:
        machine.stop()
    machine = Machine(args, data, image, folder)
    try:
        machine.until(b'Magenta> ')
        machine.send('0', b'Power> ')
        machine.send('3', b'to confirm: ')
        machine.send('RESCUE', b'avalanche> ')
        report('writeback survives cold restart', lambda: contains(machine.command('cat cache-check'), b'CACHE-WRITEBACK-SUCCESS'))
    finally:
        machine.stop()
    report('host e2fsck after guest reads', lambda: run(['e2fsck', '-fn', str(image)]))

if __name__ == '__main__':
    main()
