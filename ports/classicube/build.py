#!/usr/bin/env python3
"""Build the pinned ClassiCube C client as a Magenta graphical application disk."""
import argparse
import gzip
import hashlib
import os
import pathlib
import shutil
import subprocess
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ("https://github.com/ClassiCube/ClassiCube/archive/refs/tags/1.3.9.tar.gz", "5bb62dbb3ad27e1fbb09842f0cb1f4802b2686553b017e0b67ce09b36f908246")

def run(args, **kwargs): subprocess.run([str(x) for x in args], check=True, **kwargs)
def fetch(cache):
    archive = cache / "classicube-1.3.9.tar.gz"
    if not archive.exists(): run(["curl", "--fail", "--location", "--retry", "2", "--output", archive, SOURCE[0]])
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SOURCE[1]: raise ValueError("ClassiCube source checksum mismatch")
    return archive
def sources(tree):
    files = sorted(tree.glob("src/*.c")) + sorted(tree.glob("third_party/bearssl/*.c"))
    excluded = {"Window_Magenta.c", "Window_Terminal.c", "Window_SDL2.c", "Window_SDL3.c", "Window_Wayland.c", "Window_X11.c", "Window_Win.c", "Graphics_D3D11.c", "Graphics_D3D9.c", "Graphics_GL1.c", "Graphics_GL11.c", "Graphics_GL2.c", "Platform_Windows.c"}
    return [p for p in files if p.name not in excluded] + [tree / "src/Window_Magenta.c"]
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=pathlib.Path, default=ROOT / "avalanche/build/classicube")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args(); cache = args.cache.resolve(); cache.mkdir(parents=True, exist_ok=True)
    tree = cache / "ClassiCube-1.3.9"
    if not tree.exists():
        with tarfile.open(fetch(cache)) as archive: archive.extractall(cache, filter="data")
    shutil.copyfile(ROOT / "ports/classicube/Window_Magenta.c", tree / "src/Window_Magenta.c")
    core = tree / "src/Core.h"; text = core.read_text()
    core.write_text(text.replace("#ifndef CC_MAGENTA_OFFLINE\n#define CC_BUILD_NETWORKING\n#endif\n", "#define CC_BUILD_NETWORKING\n", 1))
    worker = tree / "src/Http_Worker.c"
    worker_text = worker.read_text()
    if "defined CC_BUILD_MAGENTA" not in worker_text:
        worker_text = worker_text.replace(
            "#if defined CC_BUILD_PSP || defined CC_BUILD_NDS",
            "#if defined CC_BUILD_PSP || defined CC_BUILD_NDS || defined CC_BUILD_MAGENTA")
    if "#ifndef CC_BUILD_MAGENTA" not in worker_text:
        worker_text = worker_text.replace(
        'Thread_Run(&workerThread, WorkerLoop, 128 * 1024, "HTTP");',
        '#ifndef CC_BUILD_MAGENTA\nThread_Run(&workerThread, WorkerLoop, 128 * 1024, "HTTP");\n#endif')
    worker.write_text(worker_text)
    resources = tree / "src/Resources.c"
    resource_text = resources.read_text()
    if "#ifndef CC_BUILD_MAGENTA" not in resource_text:
        resource_text = resource_text.replace("\t&mccTexsAssetSet,", "#ifndef CC_BUILD_MAGENTA\n\t&mccTexsAssetSet,\n#endif")
    resources.write_text(resource_text)
    output = cache / "classicube.mgnta"
    flags = ["-O2", "-pipe", "-std=c99", "-static", "-no-pie", "-fno-pie", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", "-DCC_BUILD_MANUAL", "-DCC_BUILD_MAGENTA", "-DCC_BUILD_NOMUSIC", "-DCC_BUILD_NOSOUNDS", "-DCC_BUILD_POSIX", "-DCC_BUILD_LINUX", "-DCC_BUILD_COOPTHREADED", "-DCC_WIN_BACKEND=CC_WIN_BACKEND_TERMINAL", "-DCC_GFX_BACKEND=CC_GFX_BACKEND_SOFTGPU", "-DCC_AUD_BACKEND=CC_AUD_BACKEND_NULL", "-DCC_NET_BACKEND=CC_NET_BACKEND_BUILTIN", "-DCC_SSL_BACKEND=CC_SSL_BACKEND_BEARSSL", "-I" + str(tree / "src"), "-I" + str(tree / "third_party/bearssl")]
    run([os.environ.get("CC", "gcc"), *flags, "-o", output, *sources(tree), "-lm"], cwd=tree)
    run([os.environ.get("STRIP", "strip"), output])
    stage = cache / "stage"; shutil.rmtree(stage, ignore_errors=True); stage.mkdir()
    shutil.copy2(output, stage / "classicube.mgnta")
    (stage / "texpacks").mkdir()
    shutil.copy2(tree / "misc/cc_textures.zip", stage / "texpacks/classicube.zip")
    shutil.copy2(tree / "license.txt", stage / "CLASSICUBE-LICENSE.txt")
    shutil.copy2(tree / "credits.txt", stage / "CLASSICUBE-CREDITS.txt")
    (stage / "README.txt").write_text("ClassiCube 1.3.9 for Magenta\n\nOpen classicube.mgnta from Files or run classicube.mgnta in Terminal.\nUse the offline launcher to create and explore local Classic worlds.\n")
    image = cache / "classicube-data.img"
    with image.open("wb") as disk: disk.truncate(256 * 1024 * 1024)
    run(["mkfs.ext4", "-q", "-F", "-b", "4096", "-d", stage, image])
    run(["e2fsck", "-fn", image])
    destination = ROOT / "Images/classicube-data.img.gz"; destination.parent.mkdir(exist_ok=True)
    with image.open("rb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=destination.open("wb"), mtime=0) as packed: shutil.copyfileobj(raw, packed)
    manifest = ROOT / "Images/SHA256SUMS"; name = "Images/classicube-data.img.gz"; rows = [r for r in manifest.read_text().splitlines() if not r.endswith("  " + name)] if manifest.exists() else []
    rows.append(hashlib.sha256(destination.read_bytes()).hexdigest() + "  " + name); manifest.write_text("\n".join(rows) + "\n")
if __name__ == "__main__": main()
