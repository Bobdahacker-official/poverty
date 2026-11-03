#!/usr/bin/env python3
"""Exercise NetSurf's real guest rendering, DOM, TCP and certificate validation."""

import argparse
import collections
import gzip
import http.server
import json
import os
import pathlib
import select
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=pathlib.Path)
parser.add_argument("--tls-only", action="store_true")
parser.add_argument("--untrusted", action="store_true")
parser.add_argument("--welcome", action="store_true")
args = parser.parse_args()
folder = pathlib.Path(tempfile.mkdtemp(prefix="netsurf-test-"))
print("NetSurf artifacts: " + str(folder), flush=True)
subprocess.run(
    ["python3", str(ROOT / "sdk/mgnta.py"), "build", str(ROOT / "sdk/examples/window")],
    check=True,
)
requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests.append((self.server.secure, self.path))
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        if self.server.secure:
            body = "<!doctype html><html><head><title>Verified HTTPS</title><style>body{background:#513781;color:white;font:22px sans-serif}</style></head><body><h1>Verified HTTPS on Magenta</h1><p>Certificate validation and encrypted TCP transport.</p></body></html>"
        else:
            body = (
                '<!doctype html><html><head><title>NetSurf on Magenta</title><style>body{background:#102d40;color:white;font:22px sans-serif}a{display:block;background:#eacc22;color:black;padding:24px;width:300px}#result{background:#602020;padding:28px}</style></head><body><h1>Avalanche NetSurf</h1><p id="result">Loading JavaScript</p><a href="https://10.0.2.2:'
                + str(httpsd.server_port)
                + '/secure">Open verified HTTPS</a><script>document.getElementById("result").setAttribute("style","background:#21613f;padding:28px");document.getElementById("result").textContent="JavaScript works";</script></body></html>'
            )
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *unused):
        pass


key, cert = folder / "key.pem", folder / "cert.pem"
subprocess.run(
    [
        "openssl",
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-days",
        "2",
        "-subj",
        "/CN=NetSurf test",
        "-addext",
        "subjectAltName=IP:10.0.2.2",
        "-keyout",
        str(key),
        "-out",
        str(cert),
    ],
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
httpsd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
httpd.secure, httpsd.secure = False, True
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
httpsd.socket = context.wrap_socket(httpsd.socket, server_side=True)
for server in (httpd, httpsd):
    threading.Thread(target=server.serve_forever, daemon=True).start()
image = folder / "data.img"
if args.root:
    stage = folder / "root"
    shutil.copytree(args.root, stage)
    with (stage / "netsurf/ca.pem").open("ab") as file:
        if not args.untrusted:
            file.write(b"\n" + cert.read_bytes())
    with image.open("wb") as file:
        file.truncate(256 * 1024**2)
    subprocess.run(
        ["mkfs.ext4", "-q", "-F", "-b", "4096", "-d", str(stage), str(image)],
        check=True,
    )
else:
    with gzip.open(ROOT / "Images/netsurf-data.img.gz", "rb") as source, image.open(
        "wb"
    ) as out:
        shutil.copyfileobj(source, out)
    ca = folder / "ca.pem"
    subprocess.run(
        ["debugfs", "-R", "dump /netsurf/ca.pem " + str(ca), str(image)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    with ca.open("ab") as file:
        if not args.untrusted:
            file.write(b"\n" + cert.read_bytes())
    for action in ["rm /netsurf/ca.pem", "write " + str(ca) + " /netsurf/ca.pem"]:
        subprocess.run(
            ["debugfs", "-w", "-R", action, str(image)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
subprocess.run(
    [
        "debugfs",
        "-w",
        "-R",
        "write "
        + str(ROOT / "sdk/examples/window/build/sdk-window.mgnta")
        + " /sdk-window.mgnta",
        str(image),
    ],
    check=True,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
command = [
    os.environ.get("QEMU", "qemu-system-x86_64"),
    "-accel",
    "tcg",
    "-cpu",
    "qemu64",
    "-m",
    "512",
    "-machine",
    "pc,vmport=on",
    "-vga",
    "std",
    "-display",
    "none",
    "-monitor",
    "none",
    "-serial",
    "stdio",
    "-no-reboot",
    "-cdrom",
    str(ROOT / ("Images/magenta.iso" if args.welcome else "Images/avalanche.iso")),
    "-boot",
    "d",
    "-drive",
    "file=" + str(image) + ",format=raw,if=ide",
    "-netdev",
    "user,id=net",
    "-device",
    "e1000,netdev=net,romfile=",
    "-device",
    "virtio-rng-pci,disable-modern=on",
    "-qmp",
    "unix:" + str(folder / "qmp.sock") + ",server=on,wait=off",
]
if os.environ.get("QEMU_BIOS"):
    command += ["-bios", os.environ["QEMU_BIOS"]]
if os.environ.get("QEMU_DATA"):
    command += ["-L", os.environ["QEMU_DATA"]]
if os.environ.get("QEMU_VGA_ROM"):
    command += ["-global", "VGA.romfile=" + os.environ["QEMU_VGA_ROM"]]
process = subprocess.Popen(
    command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
)
log, pending = bytearray(), bytearray()


def drain(timeout=0.1):
    if select.select([process.stdout], [], [], timeout)[0]:
        data = os.read(process.stdout.fileno(), 65536)
        log.extend(data)
        pending.extend(data)
        (folder / "serial.log").write_bytes(log)


def until(needle, timeout=60):
    deadline = time.monotonic() + timeout
    while needle not in pending and time.monotonic() < deadline:
        drain()
    if needle not in pending:
        raise AssertionError(pending[-6000:].decode(errors="replace"))
    del pending[: pending.index(needle) + len(needle)]


def send(text):
    process.stdin.write(text.encode() + b"\n")
    process.stdin.flush()


connection = None
try:
    if not args.welcome:
        until(b"avalanche> ")
        send("linuxtrace on")
        until(b"avalanche> ")
        send("gui")
    until(b"Magenta graphical desktop ready")
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(30)
    connection.connect(str(folder / "qmp.sock"))
    stream = connection.makefile("rwb", buffering=0)
    stream.readline()

    def qmp(name, arguments=None):
        request = {"execute": name}
        if arguments:
            request["arguments"] = arguments
        stream.write(json.dumps(request).encode() + b"\n")
        while True:
            response = json.loads(stream.readline())
            if "error" in response:
                raise RuntimeError(response)
            if "return" in response:
                return response["return"]

    qmp("qmp_capabilities")
    qmp("human-monitor-command", {"command-line": "sendkey ctrl-t 50"})
    until(b"user> ")
    target = (
        "https://10.0.2.2:" + str(httpsd.server_port) + "/secure"
        if args.tls_only
        else "http://10.0.2.2:" + str(httpd.server_port) + "/index"
    )
    send("netsurf.mgnta" if args.welcome else "netsurf.mgnta -v -f avalanche " + target)

    def screenshot():
        path = folder / "screen.ppm"
        qmp("screendump", {"filename": str(path)})
        return Image.open(path).convert("RGB")

    def kernel_value(name, fmt):
        symbols = {}
        for line in subprocess.check_output(
            ["nm", str(ROOT / "avalanche/build/avalanche.elf")], text=True
        ).splitlines():
            fields = line.split()
            if len(fields) == 3:
                symbols[fields[2]] = int(fields[0], 16)
        address = symbols[name]
        if address >= 0xFFFF800000000000:
            address -= 0xFFFF800000000000
        path = folder / "state.bin"
        qmp(
            "human-monitor-command",
            {"command-line": f'pmemsave {address:#x} {struct.calcsize(fmt)} "{path}"'},
        )
        return struct.unpack(fmt, path.read_bytes())

    def wait_color(color, minimum, name, timeout=75):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            drain(0.2)
            screen = screenshot()
            if (
                collections.Counter(
                    screen.get_flattened_data()
                    if hasattr(screen, "get_flattened_data")
                    else screen.getdata()
                )[color]
                >= minimum
            ):
                print("PASS " + name, flush=True)
                return screen
        raise AssertionError(name + "\n" + log[-5000:].decode(errors="replace"))

    if args.welcome:
        screen = wait_color(
            (23, 20, 33),
            30000,
            "Magenta boot and NetSurf default surface and welcome page",
        )
        screen.save(folder / "welcome.png")
        raise SystemExit(0)
    if args.tls_only:
        if args.untrusted:
            until(b"content_open: content", 75)
            until(b"about:query/ssl", 75)
            assert not any(secure for secure, path in requests), requests
            print(
                "PASS HTTPS rejects an untrusted certificate before sending HTTP",
                flush=True,
            )
            raise SystemExit(0)
        screen = wait_color(
            (81, 55, 129),
            30000,
            "HTTPS certificate validation and encrypted page rendering",
        )
        screen.save(folder / "https.png")
        raise SystemExit(0)
    wait_color((16, 45, 64), 30000, "HTTP HTML and CSS rendering")
    screen = wait_color((33, 97, 63), 10000, "JavaScript DOM mutation renders in guest")
    screen.save(folder / "http.png")
    points = [
        (x, y)
        for y in range(screen.height)
        for x in range(screen.width)
        if screen.getpixel((x, y)) == (234, 204, 34)
    ]
    assert points, "HTTPS link missing"
    x = (min(p[0] for p in points) + max(p[0] for p in points)) // 2
    y = (min(p[1] for p in points) + max(p[1] for p in points)) // 2

    def click(x, y):
        qmp(
            "input-send-event",
            {
                "events": [
                    {
                        "type": "abs",
                        "data": {
                            "axis": "x",
                            "value": round(x * 32767 / (screen.width - 1)),
                        },
                    },
                    {
                        "type": "abs",
                        "data": {
                            "axis": "y",
                            "value": round(y * 32767 / (screen.height - 1)),
                        },
                    },
                ]
            },
        )
        time.sleep(0.25)
        for down in (True, False):
            qmp(
                "input-send-event",
                {"events": [{"type": "btn", "data": {"button": "left", "down": down}}]},
            )
            time.sleep(0.6)

    click(x, y)
    screen = wait_color(
        (81, 55, 129),
        30000,
        "HTTPS certificate validation and encrypted page rendering",
    )
    screen.save(folder / "https.png")
    assert (True, "/secure") in requests, requests
    print("PASS pointer click navigates browser link", flush=True)

    def close_window():
        x, y, w, h = kernel_value("client_bounds", "ffff")
        (scale,) = kernel_value("native_scale", "f")
        click((x + w - 36) * scale, (y + 28) * scale)
        until(b"user> ")

    close_window()
    print("PASS browser window close exits the process", flush=True)
    send("sdk-window.mgnta")
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        drain(0.2)
        if kernel_value("gui_clients", "i")[0]:
            break
    else:
        raise AssertionError("SDK window did not open")
    time.sleep(1)
    x, y, w, h = kernel_value("client_bounds", "ffff")
    (scale,) = kernel_value("native_scale", "f")
    crop = tuple(
        round(value * scale) for value in (x + 20, y + 60, x + w - 20, y + h - 20)
    )

    def white_count():
        view = screenshot().crop(crop)
        pixels = (
            view.get_flattened_data()
            if hasattr(view, "get_flattened_data")
            else view.getdata()
        )
        return collections.Counter(pixels)[(255, 255, 255)]

    before_white = white_count()
    click((x + w / 2) * scale, (y + h / 2) * scale)
    # Move the compositor pointer away so it cannot cover the painted pixels.
    qmp(
        "input-send-event",
        {
            "events": [
                {"type": "abs", "data": {"axis": "x", "value": 0}},
                {"type": "abs", "data": {"axis": "y", "value": 16384}},
            ]
        },
    )
    time.sleep(0.3)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        drain(0.2)
        if white_count() - before_white >= 50:
            print("PASS SDK native GUI draw and mouse event response", flush=True)
            break
    else:
        raise AssertionError("SDK window did not draw its mouse response")
    close_window()
    print("PASS SDK native window close and process cleanup", flush=True)
finally:
    (folder / "serial.log").write_bytes(log)
    if connection:
        connection.close()
    process.terminate()
    process.wait(timeout=5)
    httpd.shutdown()
    httpsd.shutdown()
subprocess.run(["e2fsck", "-fn", str(image)], check=True, stdout=subprocess.DEVNULL)
print("PASS NetSurf and SDK window activity leave ext4 clean", flush=True)
