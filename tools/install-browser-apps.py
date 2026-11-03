#!/usr/bin/env python3
"""Install bundled graphical programs into the stopped browser VM's ext4 disk."""
import gzip
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

images, disk, runtime = map(Path, sys.argv[1:])


def debug(image, command, write=False):
    result = subprocess.run(
        ["debugfs", *(["-w"] if write else []), "-R", command, str(image)],
        capture_output=True, text=True, check=True)
    return result.stdout + result.stderr


def quoted(path):
    return '"' + str(path).replace('\\', '\\\\').replace('"', '\\"') + '"'


for app in ("classicube", "netsurf"):
    archive = images / (app + "-data.img.gz")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    marker = runtime / (app + ".installed.sha256")
    binary = "/" + app + ".mgnta"
    if (marker.exists() and marker.read_text().strip() == digest and
            "Inode:" in debug(disk, "stat " + binary)):
        continue
    with tempfile.TemporaryDirectory(prefix=app + "-install-", dir=runtime) as folder:
        stage = Path(folder)
        source = stage / "source.img"
        with gzip.open(archive, "rb") as incoming, source.open("wb") as outgoing:
            shutil.copyfileobj(incoming, outgoing)
        files = stage / "files"
        files.mkdir()
        debug(source, "rdump / " + quoted(files))
        executable = files / (app + ".mgnta")
        if not executable.is_file() or executable.read_bytes()[:4] != b"\x7fELF":
            raise RuntimeError("Missing executable in " + str(archive))
        paths = [executable]
        if app == "netsurf":
            resources = files / "netsurf"
            if not (resources / "ca.pem").is_file():
                raise RuntimeError("NetSurf resources are missing")
            paths += sorted(resources.rglob("*"), key=lambda p: (len(p.parts), str(p)))
            paths.insert(1, resources)
        for path in paths:
            target = "/" + path.relative_to(files).as_posix()
            if path.is_dir():
                debug(disk, "mkdir " + quoted(target), True)
            elif path.is_file():
                debug(disk, "rm " + quoted(target), True)
                debug(disk, "write " + quoted(path) + " " + quoted(target), True)
                check = stage / "verify"
                check.unlink(missing_ok=True)
                debug(disk, "dump " + quoted(target) + " " + quoted(check))
                if not check.exists() or check.read_bytes() != path.read_bytes():
                    raise RuntimeError("Disk installation failed for " + target)
        marker.write_text(digest + "\n")
        print("Installed and verified " + app + " on " + str(disk), flush=True)
