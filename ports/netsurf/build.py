#!/usr/bin/env python3
"""Build NetSurf's Avalanche surface, static HTTP/TLS transport and writable app disk."""

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
SOURCES = {
    "netsurf-all-3.11.tar.gz": (
        "https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz",
        "4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b",
    ),
    "curl-8.14.1.tar.xz": (
        "https://curl.se/download/curl-8.14.1.tar.xz",
        "f4619a1e2474c4bbfedc88a7c2191209c8334b48fa1f4e53fd584cc12e9120dd",
    ),
    "libiconv-1.18.tar.gz": (
        "https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.18.tar.gz",
        "3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8",
    ),
    "freetype-2.13.3.tar.xz": (
        "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz",
        "0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289",
    ),
    "ca.pem": (
        "https://curl.se/ca/cacert-2025-05-20.pem",
        "ab3ee3651977a4178a702b0b828a4ee7b2bbb9127235b0ab740e2e15974bf5db",
    ),
}


def run(args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def record_checksum(path):
    manifest = ROOT / "Images/SHA256SUMS"
    name = path.relative_to(ROOT).as_posix()
    records = manifest.read_text().splitlines() if manifest.exists() else []
    records = [line for line in records if line.split(maxsplit=1)[-1] != name]
    records.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + name)
    manifest.write_text("\n".join(records) + "\n")


def fetch(cache, name):
    path = cache / name
    url, digest = SOURCES[name]
    if not path.exists():
        temporary = path.with_suffix(path.suffix + ".partial")
        run(
            ["curl", "--fail", "--location", "--retry", "2", "--output", temporary, url]
        )
        temporary.replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError("Source checksum mismatch: " + name)
    return path


def unpack(archive, cache, folder):
    target = cache / folder
    if not target.exists():
        with tarfile.open(archive) as file:
            file.extractall(cache, filter="data")
    return target


def replace(path, old, new):
    text = path.read_text()
    if new not in text:
        if old not in text:
            raise ValueError("Upstream source did not match: " + str(path))
        path.write_text(text.replace(old, new, 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cache", type=pathlib.Path, default=ROOT / "avalanche/build/netsurf"
    )
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument(
        "--fonts",
        type=pathlib.Path,
        default=pathlib.Path("/usr/share/fonts/truetype/dejavu"),
    )
    args = parser.parse_args()
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    browser = unpack(fetch(cache, "netsurf-all-3.11.tar.gz"), cache, "netsurf-all-3.11")
    curl = unpack(fetch(cache, "curl-8.14.1.tar.xz"), cache, "curl-8.14.1")
    ca = fetch(cache, "ca.pem")
    prefix = cache / "curl-install"
    iconv = unpack(fetch(cache, "libiconv-1.18.tar.gz"), cache, "libiconv-1.18")
    iconv_prefix = cache / "iconv-install"
    freetype = unpack(fetch(cache, "freetype-2.13.3.tar.xz"), cache, "freetype-2.13.3")
    freetype_prefix = cache / "freetype-install"
    if not (freetype_prefix / "lib/libfreetype.a").exists():
        run(
            [
                freetype / "configure",
                "--prefix=" + str(freetype_prefix),
                "--disable-shared",
                "--enable-static",
                "--without-harfbuzz",
                "--without-bzip2",
                "--without-png",
                "--without-brotli",
            ],
            cwd=freetype,
        )
        run(["make", "-j" + str(args.jobs)], cwd=freetype)
        run(["make", "install"], cwd=freetype)
    if not (iconv_prefix / "lib/libiconv.a").exists():
        run(
            [
                iconv / "configure",
                "--prefix=" + str(iconv_prefix),
                "--disable-shared",
                "--enable-static",
                "--disable-nls",
            ],
            cwd=iconv,
        )
        run(["make", "-j" + str(args.jobs)], cwd=iconv)
        run(["make", "install"], cwd=iconv)
    if not (prefix / "lib/libcurl.a").exists():
        run(
            [
                curl / "configure",
                "--prefix=" + str(prefix),
                "--disable-shared",
                "--enable-static",
                "--with-openssl",
                "--without-libpsl",
                "--without-libidn2",
                "--without-brotli",
                "--without-zstd",
                "--without-libssh2",
                "--without-nghttp2",
                "--without-nghttp3",
                "--without-ngtcp2",
                "--without-librtmp",
                "--disable-ldap",
                "--disable-ldaps",
                "--disable-threaded-resolver",
                "--disable-unix-sockets",
                "--disable-socketpair",
                "--disable-manual",
                "--disable-docs",
                "--with-ca-bundle=/netsurf/ca.pem",
            ],
            cwd=curl,
        )
        run(["make", "-j" + str(args.jobs)], cwd=curl)
        run(["make", "install"], cwd=curl)
    replace(
        browser / "libnsfb/include/libnsfb.h",
        "    NSFB_SURFACE_COUNT,",
        "    NSFB_SURFACE_AVALANCHE,\n    NSFB_SURFACE_COUNT,",
    )
    replace(
        browser / "libnsfb/src/surface/Makefile",
        "surface.c ram.c",
        "surface.c ram.c avalanche.c",
    )
    shutil.copyfile(
        ROOT / "ports/netsurf/avalanche.c", browser / "libnsfb/src/surface/avalanche.c"
    )
    shutil.copyfile(
        ROOT / "avalanche/include/magenta.h", browser / "libnsfb/include/magenta.h"
    )
    shutil.copyfile(
        ROOT / "ports/netsurf/resolver.c",
        browser / "netsurf/frontends/framebuffer/avalanche_resolver.c",
    )
    replace(
        browser / "netsurf/frontends/framebuffer/Makefile",
        "S_FRONTEND += font_",
        "S_FRONTEND += avalanche_resolver.c\nS_FRONTEND += font_",
    )
    backend = browser / "netsurf/content/fetchers/file/file.c"
    backend.write_text(
        backend.read_text().replace(
            "#ifdef HAVE_MMAP", "#if defined(HAVE_MMAP) && !defined(__AVALANCHE__)"
        )
    )
    if (
        "X509_VERIFY_PARAM_set1_ip_asc"
        not in (browser / "netsurf/content/fetchers/curl.c").read_text()
    ):
        with (ROOT / "ports/netsurf/tls-host.patch").open("rb") as patch:
            run(["patch", "-p1"], cwd=browser, stdin=patch)
    replace(
        browser / "netsurf/frontends/framebuffer/gui.c",
        "nsfb_enumerate_surface_types(framebuffer_pick_default_fename, NULL);",
        'nsfb_enumerate_surface_types(framebuffer_pick_default_fename, NULL);\n#ifdef __AVALANCHE__\n\tfename = "avalanche";\n#endif',
    )
    framebuffer = browser / "netsurf/frontends/framebuffer"
    for source in (ROOT / "ports/netsurf/avalanche_download.inc",
                   ROOT / "avalanche/include/magenta.h"):
        shutil.copyfile(source, framebuffer / source.name)
    replace(
        framebuffer / "gui.c",
        "static struct gui_misc_table framebuffer_misc_table = {",
        '#include "avalanche_download.inc"\n\n'
        "static struct gui_misc_table framebuffer_misc_table = {",
    )
    replace(
        framebuffer / "gui.c",
        "ret = netsurf_register(&framebuffer_table);",
        "framebuffer_table.download = &magenta_download_table;\n"
        "\tret = netsurf_register(&framebuffer_table);",
    )
    (browser / "netsurf/Makefile.config").write_text(
        "NETSURF_FB_RESPATH := /netsurf\nNETSURF_FB_FONTLIB := freetype\nNETSURF_FB_FONTPATH := /netsurf/fonts\nNETSURF_FB_FONT_CURSIVE := DejaVuSerif.ttf\nNETSURF_FB_FONT_FANTASY := DejaVuSans-Bold.ttf\nCFLAGS += -D__AVALANCHE__\nLDFLAGS += -static -no-pie $(shell pkg-config --static --libs libcurl) -L"
        + str(iconv_prefix / "lib")
        + " -liconv\n"
    )
    (browser / "inst-framebuffer/build-stamp").unlink(missing_ok=True)
    environment = dict(os.environ)
    environment["PATH"] = str(prefix / "bin") + os.pathsep + environment.get("PATH", "")
    environment["PKG_CONFIG_PATH"] = (
        str(freetype_prefix / "lib/pkgconfig")
        + os.pathsep
        + str(prefix / "lib/pkgconfig")
        + os.pathsep
        + environment.get("PKG_CONFIG_PATH", "")
    )
    environment["CPATH"] = (
        str(iconv_prefix / "include")
        + os.pathsep
        + str(prefix / "include")
        + os.pathsep
        + environment.get("CPATH", "")
    )
    environment["LIBRARY_PATH"] = (
        str(prefix / "lib") + os.pathsep + environment.get("LIBRARY_PATH", "")
    )
    run(
        ["make", "-j" + str(args.jobs), "TARGET=framebuffer"],
        cwd=browser,
        env=environment,
    )
    output = ROOT / "Images/netsurf-data.img.gz"
    output.parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="netsurf-disk-", dir=cache) as temporary:
        folder = pathlib.Path(temporary)
        stage = folder / "root"
        stage.mkdir()
        apps = [
            "magenta",
            "ush",
            "hello",
            "cat",
            "udp",
            "http",
            "worker",
            "linuxhello",
            "linuxfork",
            "aplay",
        ]
        run(
            [
                "make",
                "-C",
                ROOT / "avalanche",
                *["build/user/" + name + ".elf" for name in apps],
            ]
        )
        for name in apps:
            shutil.copyfile(
                ROOT / "avalanche/build/user" / (name + ".elf"), stage / name
            )
        for source in (ROOT / "magenta/files").iterdir():
            if source.is_file():
                shutil.copyfile(source, stage / source.name)
        shutil.copytree(
            browser / "netsurf/frontends/framebuffer/res", stage / "netsurf"
        )
        shutil.copyfile(
            browser / "netsurf/frontends/framebuffer/res/en/Messages",
            stage / "netsurf/Messages",
        )
        shutil.copyfile(ca, stage / "netsurf/ca.pem")
        fonts = stage / "netsurf/fonts"
        fonts.mkdir(exist_ok=True)
        for name in [
            "DejaVuSans.ttf",
            "DejaVuSans-Bold.ttf",
            "DejaVuSans-Oblique.ttf",
            "DejaVuSans-BoldOblique.ttf",
            "DejaVuSerif.ttf",
            "DejaVuSerif-Bold.ttf",
            "DejaVuSansMono.ttf",
            "DejaVuSansMono-Bold.ttf",
        ]:
            shutil.copyfile(args.fonts / name, fonts / name)
        shutil.copyfile(browser / "netsurf/nsfb", stage / "netsurf.mgnta")
        run([os.environ.get("STRIP", "strip"), stage / "netsurf.mgnta"])
        (stage / "netsurf/Choices").write_text(
            "max_fetchers:1\nmax_fetchers_per_host:1\nenable_javascript:1\nhomepage_url:file:///netsurf/index.html\n"
        )
        (stage / "netsurf/index.html").write_text(
            '<!doctype html><html><head><meta charset="utf-8"><title>NetSurf on Magenta</title><style>body{background:#171421;color:#ebe4ef;font-family:sans-serif;padding:30px}h1{color:#ff72bc}a{color:#ff98cd}</style></head><body><h1>NetSurf on Magenta</h1><p>Browse with NetSurf on Avalanche Kernel.</p><p>Enter an HTTP or HTTPS address in the toolbar.</p><p><a href="https://bobdahacker-official.github.io/poverty/">Magenta website</a> · <a href="https://www.netsurf-browser.org/">NetSurf project</a></p></body></html>'
        )
        licenses = stage / "netsurf/licenses"
        licenses.mkdir()
        for component in browser.iterdir():
            if not component.is_dir():
                continue
            for filename in ["COPYING", "LICENSE", "COPYING.LIB"]:
                source = component / filename
                if source.is_file():
                    shutil.copyfile(
                        source, licenses / (component.name + "-" + filename)
                    )
        shutil.copyfile(curl / "COPYING", licenses / "curl-COPYING")
        for filename in ["COPYING", "COPYING.LIB"]:
            shutil.copyfile(iconv / filename, licenses / ("libiconv-" + filename))
        shutil.copyfile(freetype / "LICENSE.TXT", licenses / "freetype-LICENSE.TXT")
        for filename in ["FTL.TXT", "GPLv2.TXT"]:
            shutil.copyfile(
                freetype / "docs" / filename, licenses / ("freetype-" + filename)
            )
        shutil.copyfile(
            ROOT / "avalanche/assets/FONT-LICENSE", licenses / "DejaVu-LICENSE"
        )
        system_docs = pathlib.Path("/usr/share/doc")
        for pattern in ["libssl-dev", "zlib1g-dev", "libpng*", "libjpeg*"]:
            found = list(system_docs.glob(pattern + "/copyright"))
            if not found:
                raise ValueError("Missing dependency license: " + pattern)
            for source in found:
                shutil.copyfile(source, licenses / (source.parent.name + "-copyright"))
        shutil.copyfile(
            "/usr/share/common-licenses/Apache-2.0", licenses / "Apache-2.0"
        )
        (licenses / "sources.txt").write_text(
            "\n".join(url + " SHA-256 " + digest for url, digest in SOURCES.values())
            + "\nAvalanche backend: https://github.com/Bobdahacker-official/poverty/tree/main/ports/netsurf\n"
        )
        image = folder / "netsurf-data.img"
        with image.open("wb") as file:
            file.truncate(256 * 1024**2)
        run(
            [
                "mkfs.ext4",
                "-q",
                "-F",
                "-b",
                "4096",
                "-O",
                "extent,dir_index,64bit,flex_bg,has_journal",
                "-d",
                stage,
                image,
            ]
        )
        run(["e2fsck", "-fn", image])
        with image.open("rb") as source, output.open(
            "wb"
        ) as destination, gzip.GzipFile(
            filename="", mode="wb", fileobj=destination, mtime=0
        ) as compressed:
            shutil.copyfileobj(source, compressed)
    record_checksum(output)
    print(output)


if __name__ == "__main__":
    main()
