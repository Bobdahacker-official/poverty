#!/usr/bin/env python3
import pathlib
import shutil
import tarfile
import tempfile
import gzip
root=pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as temporary:
    target=pathlib.Path(temporary)/'magenta-sdk';target.mkdir()
    for name in ['mgnta.py','bin','examples','include']:
        source=root/name
        if source.is_dir():shutil.copytree(source,target/name)
        else:shutil.copyfile(source,target/name)
    (target/'runtime').mkdir()
    for name in ['lib.c','lib.h','crt.S','linker.ld']:
        shutil.copyfile(root.parent/'avalanche/user'/name,target/'runtime'/name)
    for name in ['abi.h','magenta.h']:
        shutil.copyfile(root.parent/'avalanche/include'/name,target/'include'/name)
    output=root.parent/'Images/magenta-sdk.tar.gz'
    def metadata(info):
        info.mtime=0;info.uid=info.gid=0;info.uname=info.gname='';return info
    with output.open('wb') as file,gzip.GzipFile(filename='',mode='wb',fileobj=file,mtime=0) as compressed:
        with tarfile.open(fileobj=compressed,mode='w') as archive:archive.add(target,arcname='magenta-sdk',filter=metadata)
    print(output)
