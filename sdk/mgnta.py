#!/usr/bin/env python3
"""Build, sign and verify native Magenta applications."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import zipfile

ROOT=pathlib.Path(__file__).resolve().parent
REPO=ROOT.parent
INCLUDE=ROOT/'include'
RUNTIME=ROOT/'runtime'
if not RUNTIME.exists():
    RUNTIME=REPO/'avalanche/user'
    SYSTEM_INCLUDE=REPO/'avalanche/include'
else:
    SYSTEM_INCLUDE=INCLUDE
def run(command,**kwargs):return subprocess.run(list(map(str,command)),check=True,**kwargs)
def name(value):
    if not re.fullmatch(r'[a-z][a-z0-9_-]{0,20}',value):raise ValueError('Use a lowercase app name of 1–21 letters, digits, underscores or hyphens')
    return value
def certificate_key(key,cert):
    modulus=subprocess.check_output(['openssl','rsa','-in',str(key),'-noout','-modulus'],stderr=subprocess.PIPE)
    bits=int(modulus.decode().strip().split('=',1)[1],16).bit_length()
    if bits<2048 or bits>4096:raise ValueError('Use an RSA signing key between 2048 and 4096 bits')
    public=subprocess.check_output(['openssl','pkey','-in',str(key),'-pubout','-outform','DER'])
    cert_public=subprocess.check_output(['openssl','x509','-in',str(cert),'-pubkey','-noout'])
    actual=subprocess.check_output(['openssl','pkey','-pubin','-outform','DER'],input=cert_public)
    if public!=actual:raise ValueError('Signing key and certificate do not match')
def elf(path):
    data=path.read_bytes()
    if len(data)<64 or data[:7]!=b'\x7fELF\x02\x01\x01' or int.from_bytes(data[18:20],'little')!=62:
        raise ValueError('Expected an x86-64 ELF executable')
    if int.from_bytes(data[16:18],'little')!=2:raise ValueError('Expected an ET_EXEC executable')
    if len(data)>65536:raise ValueError('Signed packages currently hold executables up to 65536 bytes')
    return data
CAPABILITIES = {'read':1, 'write':2, 'network':4, 'exec':8, 'gui':16,
                'audio':32, 'clipboard-read':64, 'clipboard-write':128,
                'processes':256, 'admin':512, 'exec-memory':1024}
def capability_mask(value):
    if isinstance(value, int):
        if value < 0 or value & ~2047: raise ValueError('Invalid capability mask')
        return value
    if value == "none": return 0
    if value == "all": return 2047
    values = value.split(',') if isinstance(value, str) else value
    mask = 0
    for item in values:
        item = item.strip()
        if not item: continue
        if item not in CAPABILITIES: raise ValueError('Unknown capability: ' + item)
        mask |= CAPABILITIES[item]
    return mask
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('init');p.add_argument('directory',type=pathlib.Path)
    p=sub.add_parser('build');p.add_argument('project',type=pathlib.Path,nargs='?',default=pathlib.Path('.'))
    p=sub.add_parser('keygen');p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--identity',required=True)
    p=sub.add_parser('sign');p.add_argument('executable',type=pathlib.Path);p.add_argument('--key',type=pathlib.Path,required=True);p.add_argument('--certificate',type=pathlib.Path,required=True);p.add_argument('--version',default='1.0.0');p.add_argument('--capabilities');p.add_argument('--depends',default='');p.add_argument('--out',type=pathlib.Path,required=True)
    p=sub.add_parser('verify');p.add_argument('manifest',type=pathlib.Path);p.add_argument('--certificate',type=pathlib.Path,required=True)
    args=parser.parse_args()
    if args.command=='init':
        project=args.directory;app=name(project.name);project.mkdir(parents=True,exist_ok=False)
        shutil.copyfile(ROOT/'examples/hello/main.c',project/'main.c')
        (project/'magenta.json').write_text(json.dumps({'name':app,'version':'1.0.0','sources':['main.c']},indent=2)+'\n')
        print(project.resolve())
    elif args.command=='build':
        project=args.project.resolve();config=json.loads((project/'magenta.json').read_text());app=name(config['name'])
        output=project/'build';output.mkdir(exist_ok=True)
        flags=['-m64','-mno-red-zone','-std=c11','-Os','-Wall','-Wextra','-Werror','-ffreestanding','-fno-pie','-fno-pic','-fno-stack-protector','-fno-builtin','-fno-asynchronous-unwind-tables','-mno-sse','-mno-mmx','-msoft-float','-I'+str(INCLUDE),'-I'+str(SYSTEM_INCLUDE),'-I'+str(RUNTIME)]
        objects=[]
        sources=[*(project/s for s in config['sources']),RUNTIME/'lib.c',RUNTIME/'crt.S']
        for i,source in enumerate(sources):
            obj=output/f'{i}.o';run([os.environ.get('CC','gcc'),*flags,'-c',source,'-o',obj]);objects.append(obj)
        target=output/(app+'.mgnta')
        run([os.environ.get('LD','ld'),'-m','elf_x86_64','-T',RUNTIME/'linker.ld','-nostdlib','-o',target,*objects])
        elf(target)
        mask=capability_mask(config.get('capabilities', ['read','gui']))
        target.with_suffix('.mgnta.capabilities').write_text(str(mask)+'\n')
        print(target)
    elif args.command=='keygen':
        if any(c in args.identity for c in '/\r\n'):raise ValueError('Identity must be a single common name')
        args.out.mkdir(parents=True,exist_ok=True)
        key=args.out/'private.pem';cert=args.out/'certificate.pem'
        if key.exists() or cert.exists():raise ValueError('Signing identity already exists')
        key.touch(mode=0o600,exist_ok=False)
        run(['openssl','req','-x509','-newkey','rsa:3072','-nodes','-sha256','-keyout',key,'-out',cert,'-subj','/CN='+args.identity,'-days','3650'])
        run(['openssl','x509','-in',cert,'-outform','DER','-out',args.out/'trust.der'])
        print(hashlib.sha256((args.out/'trust.der').read_bytes()).hexdigest())
    elif args.command=='sign':
        executable=args.executable.resolve();data=elf(executable)
        if executable.suffix!='.mgnta':raise ValueError('Executable must end in .mgnta')
        name(executable.stem)
        if not re.fullmatch(r'\d+\.\d+\.\d+',args.version):raise ValueError('Version must have three numeric components')
        if args.depends:name(args.depends)
        certificate_key(args.key,args.certificate)
        args.out.mkdir(parents=True,exist_ok=True)
        target=args.out/executable.name
        if target.resolve()!=executable:shutil.copyfile(executable,target)
        sidecar=executable.with_suffix('.mgnta.capabilities')
        permissions=capability_mask(args.capabilities) if args.capabilities is not None else capability_mask(int(sidecar.read_text().strip())) if sidecar.exists() else capability_mask('read,gui')
        manifest=args.out/(executable.name+'.manifest')
        manifest.write_text(f'name {executable.name}\nversion {args.version}\ndepends {args.depends}\nfile {executable.name}\nsha256 {hashlib.sha256(data).hexdigest()}\ncapabilities {permissions}\n')
        run(['openssl','dgst','-sha256','-sign',args.key,'-out',str(manifest)+'.sig',manifest])
        run(['openssl','x509','-in',args.certificate,'-outform','DER','-out',args.out/'trust.der'])
        bundle=args.out/(executable.name+'.zip')
        with zipfile.ZipFile(bundle,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            for path in [target,manifest,pathlib.Path(str(manifest)+'.sig'),args.out/'trust.der']:
                archive.write(path,path.name)
        print(manifest)
    else:
        manifest=args.manifest.resolve();lines={}
        for line in manifest.read_text().splitlines():
            key,_,value=line.partition(' ')
            if key in lines:raise ValueError('Duplicate manifest field')
            lines[key]=value
        if 'capabilities' in lines: capability_mask(int(lines['capabilities']))
        filename=lines.get('file','');app=filename.removesuffix('.mgnta');name(app)
        if filename!=app+'.mgnta' or lines.get('name')!=filename:raise ValueError('Invalid executable name')
        data=elf(manifest.parent/filename)
        if hashlib.sha256(data).hexdigest()!=lines.get('sha256'):raise ValueError('Executable SHA-256 mismatch')
        with tempfile.TemporaryDirectory() as folder:
            public=pathlib.Path(folder)/'public.pem'
            fmt=['-inform','DER'] if args.certificate.suffix=='.der' else []
            public.write_bytes(subprocess.check_output(['openssl','x509',*fmt,'-in',str(args.certificate),'-pubkey','-noout']))
            run(['openssl','dgst','-sha256','-verify',public,'-signature',str(manifest)+'.sig',manifest])
        print('Verified '+filename)
if __name__=='__main__':
    try:main()
    except (ValueError,OSError,subprocess.CalledProcessError) as error:raise SystemExit(str(error))
