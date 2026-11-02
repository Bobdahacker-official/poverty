#!/usr/bin/env python3
import hashlib
import pathlib
import shutil
import argparse
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--signing-key',type=pathlib.Path)
parser.add_argument('--certificate',type=pathlib.Path)
args=parser.parse_args()
if bool(args.signing_key) != bool(args.certificate):
    parser.error('--signing-key and --certificate must be supplied together')
if args.signing_key:
    key_public=subprocess.check_output(['openssl','pkey','-in',str(args.signing_key),
                                        '-pubout','-outform','DER'])
    certificate_public=subprocess.check_output(['openssl','x509','-in',str(args.certificate),
                                                '-pubkey','-noout'])
    certificate_public=subprocess.check_output(['openssl','pkey','-pubin','-outform','DER'],
                                               input=certificate_public)
    if key_public != certificate_public:
        parser.error('the signing key does not match the certificate')

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
    if args.signing_key:
        subprocess.run(['openssl','dgst','-sha256','-sign',str(args.signing_key.resolve()),
                        '-out',str(destination/f'{name}.manifest.sig'),
                        str(destination/f'{name}.manifest')],check=True)
    print(f'Packaged {name} 1.0.0 {digest}')
if args.certificate:
    subprocess.run(['openssl','x509','-in',str(args.certificate.resolve()),
                    '-outform','DER','-out',str(destination/'trust.der')],check=True)
