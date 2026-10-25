#!/usr/bin/env python3
import hashlib
import pathlib
import shutil

root=pathlib.Path(__file__).resolve().parents[1]
destination=root/'build/packages'
destination.mkdir(parents=True,exist_ok=True)
dependencies={'hello':'','cat':'','worker':'','udp':'hello','http':'udp'}
for name,dependency in dependencies.items():
    source=root/'build/user'/f'{name}.elf'
    payload=source.read_bytes()
    if payload[:5]!=b'\x7fELF\x02':
        raise ValueError(f'{source} is not a native ELF64 program')
    digest=hashlib.sha256(payload).hexdigest()
    manifest=f'name {name}\nversion 1.0.0\ndepends {dependency}\nfile {name}\nsha256 {digest}\n'
    shutil.copyfile(source,destination/name)
    (destination/f'{name}.manifest').write_text(manifest)
    print(f'Packaged {name} 1.0.0 {digest}')
