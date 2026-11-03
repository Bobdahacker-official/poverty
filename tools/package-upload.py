#!/usr/bin/env python3
"""Receive and verify developer-signed app bundles for the Magenta package manager."""
import argparse
import email.parser
import email.policy
import hashlib
import html
import http.server
import io
import json
import pathlib
import secrets
import shutil
import subprocess
import tempfile
import threading
import urllib.parse
import zipfile

REPO=pathlib.Path(__file__).resolve().parents[1]
PAGE='''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Install a Magenta app</title>
<style>body{background:#12121d;color:#eceaf4;font:17px system-ui;max-width:660px;margin:8vh auto;padding:24px}h1{font-size:42px;letter-spacing:-1.5px}label{display:block;margin:32px 0 12px}input,button{font:inherit;padding:14px;border:1px solid #464054;border-radius:12px;background:#252334;color:inherit}button{background:#e6007e;cursor:pointer}p{line-height:1.7;color:#c3bdce}code{color:#ff8acb}a{color:#ff8acb}</style>
<h1>Your app. On Magenta.</h1><p>Choose the signed <code>.mgnta.zip</code> bundle created by the SDK’s <code>sign</code> command. The executable hash, manifest signature and signing certificate are verified before the app is made available to Magenta.</p>
<form action="/upload" method="post" enctype="multipart/form-data"><input type="hidden" name="token" value="TOKEN"><label for="bundle">Signed application bundle</label><input id="bundle" name="bundle" type="file" accept=".zip" required><p><button>Upload and verify</button></p></form>MESSAGE
<p>In Magenta, open Packages. Choose <b>Trust uploaded signer</b> for a new developer, then <b>Install uploaded app</b>. Launch the installed <code>.mgnta</code> file from Files or Terminal.</p></html>'''
def verify_bundle(data,folder):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries=archive.infolist()
        if len(entries)!=4 or sum(e.file_size for e in entries)>75000:raise ValueError('Expected four signed package files, at most 75,000 bytes total')
        names=[e.filename for e in entries]
        if len(set(names))!=4 or any(pathlib.PurePosixPath(n).name!=n or '\\' in n for n in names):raise ValueError('Invalid bundle paths')
        manifests=[n for n in names if n.endswith('.mgnta.manifest')]
        if len(manifests)!=1:raise ValueError('Expected one .mgnta manifest')
        manifest=manifests[0];payload=manifest.removesuffix('.manifest')
        if set(names)!={manifest,manifest+'.sig',payload,'trust.der'}:raise ValueError('Bundle files do not match the manifest')
        for entry in entries:
            if entry.file_size>65536:raise ValueError('Package member is too large')
            (folder/entry.filename).write_bytes(archive.read(entry))
        subprocess.run(['python3',str(REPO/'sdk/mgnta.py'),'verify',str(folder/manifest),'--certificate',str(folder/'trust.der')],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        fingerprint=hashlib.sha256((folder/'trust.der').read_bytes()).hexdigest()
        return manifest,payload,fingerprint
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--root',type=pathlib.Path,required=True);parser.add_argument('--port',type=int,default=6082);parser.add_argument('--bind',default='127.0.0.1');args=parser.parse_args()
    args.root.mkdir(parents=True,exist_ok=True);token=secrets.token_urlsafe(32);lock=threading.Lock()
    latest=None
    pointer=args.root/'latest.json'
    if pointer.exists():latest=json.loads(pointer.read_text())
    class Handler(http.server.BaseHTTPRequestHandler):
        def reply(self,status,data,kind='text/html; charset=utf-8'):
            self.send_response(status);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.end_headers();self.wfile.write(data)
        def do_GET(self):
            path=urllib.parse.urlsplit(self.path).path
            if path=='/':return self.reply(200,PAGE.replace('TOKEN',token).replace('MESSAGE','').encode())
            with lock:current=latest.copy() if latest else None
            if path.startswith('/latest/') and current:
                file=path.removeprefix('/latest/')
                if file=='app.manifest':file=current['manifest']
                elif file=='app.manifest.sig':file=current['manifest']+'.sig'
                if file not in {current['manifest'],current['manifest']+'.sig',current['payload'],'trust.der'}:return self.reply(404,b'File not found','text/plain')
                return self.reply(200,(args.root/current['id']/file).read_bytes(),'application/octet-stream')
            self.reply(404,b'File not found','text/plain')
        def do_POST(self):
            nonlocal latest
            if self.path!='/upload':return self.reply(404,b'File not found','text/plain')
            try:
                length=int(self.headers.get('Content-Length','0'))
                if not 0<length<=2*1024*1024:raise ValueError('Upload exceeds 2 MiB')
                content_type=self.headers.get('Content-Type','')
                if not content_type.startswith('multipart/form-data;'):raise ValueError('Use the upload form')
                message=email.parser.BytesParser(policy=email.policy.default).parsebytes(('Content-Type: '+content_type+'\r\n\r\n').encode()+self.rfile.read(length))
                parts={part.get_param('name',header='content-disposition'):part.get_payload(decode=True) for part in message.iter_parts()}
                if not secrets.compare_digest(parts.get('token',b'').decode(),token):return self.reply(403,b'Reload the upload form and try again','text/plain')
                data=parts['bundle'];identity=hashlib.sha256(data).hexdigest()
                with tempfile.TemporaryDirectory(dir=args.root) as temporary:
                    staging=pathlib.Path(temporary);manifest,payload,fingerprint=verify_bundle(data,staging)
                    destination=args.root/identity
                    with lock:
                        if not destination.exists():shutil.copytree(staging,destination)
                        latest={'id':identity,'manifest':manifest,'payload':payload,'fingerprint':fingerprint}
                        temporary_pointer=args.root/'latest.tmp';temporary_pointer.write_text(json.dumps(latest));temporary_pointer.replace(pointer)
                text='<h2>Verified '+html.escape(payload)+'</h2><p>Signer SHA-256:<br><code>'+fingerprint+'</code></p>'
                self.reply(200,PAGE.replace('TOKEN',token).replace('MESSAGE',text).encode())
            except (ValueError,KeyError,OSError,zipfile.BadZipFile,subprocess.CalledProcessError) as error:
                self.reply(400,('Upload rejected: '+str(error)).encode(),'text/plain')
    http.server.ThreadingHTTPServer((args.bind,args.port),Handler).serve_forever()
if __name__=='__main__':main()
