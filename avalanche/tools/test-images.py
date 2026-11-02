#!/usr/bin/env python3
import os
import base64
import pathlib
import struct
import subprocess
import tempfile
import zlib

root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="magenta-images-") as directory:
    folder = pathlib.Path(directory)
    pixels = bytes([255,0,0, 0,255,0, 255,255,255, 255,255,0, 0,0,0, 0,0,255])
    def chunk(name, data):
        return struct.pack(">I",len(data))+name+data+struct.pack(">I",zlib.crc32(name+data))
    png = folder / "sample.png"
    png.write_bytes(b"\x89PNG\r\n\x1a\n"+
                    chunk(b"IHDR",struct.pack(">IIBBBBB",3,2,8,2,0,0,0))+
                    chunk(b"IDAT",zlib.compress(b"\0"+pixels[:9]+b"\0"+pixels[9:]))+
                    chunk(b"IEND",b""))
    bmp = folder / "sample.bmp"
    rows = b""
    for row in [pixels[9:],pixels[:9]]:
        rows += b"".join(row[x:x+3][::-1] for x in range(0,9,3))+b"\0\0\0"
    bmp.write_bytes(b"BM"+struct.pack("<IHHI",54+len(rows),0,0,54)+
                    struct.pack("<IiiHHIIIIII",40,3,2,1,24,0,len(rows),0,0,0,0)+rows)
    malformed = folder / "broken.png"
    malformed.write_bytes(png.read_bytes()[:18])
    jpeg = folder / "sample.jpg"
    # A 3 by 2 RGB fixture, encoded at quality 100 without chroma subsampling.
    jpeg.write_bytes(base64.b64decode(
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/2wBDAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAARCAACAAMDAREAAhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABoQAAIDAQEAAAAAAAAAAAAAAAYHAwUIBAL/xAAVAQEBAAAAAAAAAAAAAAAAAAAHCv/EABwRAAMBAQEAAwAAAAAAAAAAAAMFBgQCBwEICf/aAAwDAQACEQMRAD8ADjUe69vJXV+u1Gm9j6qUqoW2vdVha6WKz0K3ANeAIcPaEZFbQCYUFixfVDYsM0ddzwcFPQ0VbwVdZxQQ8vFywQR+I/NDf4Z/XvwO8/JL6KWlx4f5BZ2NT4pke09ZV+aRdFTUbxnR0Opk5fPm6XY0btWGohNO1iw1aNes5OzHMQnXXXyTentmgfSLw4mTAR3FhR0rYw9mngrSjqm2ulqH7EnJPjva7pKNs0fv2unou5w7ZMGrE+jds0HJ/9k="))
    binary = folder / "image-check"
    subprocess.run([os.environ.get("CC","gcc"),"-O1","-g","-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer","-Iavalanche/include",
                    "avalanche/tools/test-images.c","-o",str(binary)],cwd=root,check=True)
    environment = dict(os.environ)
    # The managed runner uses ptrace, which LeakSanitizer does not support.
    # The C harness separately asserts that every allocated page is released.
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    subprocess.run([str(binary),str(png),str(bmp),str(jpeg),str(malformed)],check=True,env=environment)
