#!/usr/bin/env python3
"""Create an Avalanche FS v2 disk image. Never overwrites an existing disk image."""
import pathlib
import struct
import sys
import zlib

output = pathlib.Path(sys.argv[1])
if output.exists():
    print(f"Retaining existing disk image: {output}")
    sys.exit(0)
start = 2048
sectors = 32768
image = bytearray(sectors * 512)
image[446 + 4] = 0x7F
struct.pack_into('<II', image, 446 + 8, start, sectors - start)
image[510:512] = b'\x55\xaa'
directory = bytearray(2048)
for slot, arg in enumerate(sys.argv[2:]):
    if slot >= 32:
        raise ValueError('Too many files')
    path = pathlib.Path(arg)
    data = path.read_bytes()
    if len(data) > 65536:
        raise ValueError(f'File too large: {path}')
    name = path.stem.encode('ascii')
    if not 0 < len(name) < 32:
        raise ValueError(f'Invalid name: {path}')
    struct.pack_into('<32sIIII16x', directory, slot * 64,
                     name, len(data), 0, zlib.crc32(data), 1)
    offset = (start + 10 + slot * 256) * 512
    image[offset:offset + len(data)] = data
for bank in range(2):
    header = bytearray(512)
    struct.pack_into('<8sIIIII', header, 0, b'AVLFS02\0', 2,
                     1 if bank == 0 else 0, 32, zlib.crc32(directory), 0)
    struct.pack_into('<I', header, 24, zlib.crc32(header))
    offset = (start + bank * 5) * 512
    image[offset:offset + 512] = header
    image[offset + 512:offset + 2560] = directory
# Exclusive creation preserves any existing local disk and its persistent files.
with output.open('xb') as disk:
    disk.write(image)
print(f'Created {output} ({len(image)} bytes)')
