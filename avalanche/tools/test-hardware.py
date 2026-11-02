#!/usr/bin/env python3
"""Exercise storage DMA, network adapters and persistence on two PCI chipsets."""
import os
import argparse
import pathlib
import select
import shutil
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[2]
profiles = [
    ("pc", "ATA", "pcnet", "AMD PCnet", []),
    ("pc", "AHCI", "rtl8139", "RTL8139",
     ["-device", "ich9-ahci,id=sata", "-device", "ide-hd,drive=data,bus=sata.0"]),
    ("pc", "VirtIO", "virtio-net-pci,disable-modern=on", "VirtIO legacy net",
     ["-device", "virtio-blk-pci,drive=data,disable-modern=on"]),
    ("q35", "NVMe", "e1000", "Intel E1000",
     ["-device", "nvme,drive=data,serial=AVALANCHE-TEST"]),
]
parser = argparse.ArgumentParser()
parser.add_argument("--storage", choices=[profile[1] for profile in profiles])
args = parser.parse_args()
if args.storage:
    profiles = [profile for profile in profiles if profile[1] == args.storage]
with tempfile.TemporaryDirectory(prefix="avalanche-hardware-") as directory:
    folder = pathlib.Path(directory)
    base = folder / "base.img"
    with base.open("wb") as file:
        file.truncate(64 * 1024 * 1024)
    subprocess.run(["mkfs.ext4", "-F", "-q", "-b", "4096", str(base)], check=True)
    for chipset, storage, adapter, nic, extra in profiles:
        disk = folder / (storage + ".img")
        shutil.copyfile(base, disk)
        for boot in range(2):
            command = [os.environ.get("QEMU", "qemu-system-x86_64"), "-accel", "tcg",
                       "-machine", chipset, "-m", "8192", "-boot", "d", "-vga", "none",
                       "-display", "none", "-monitor", "none", "-serial", "stdio",
                       "-no-reboot", "-cdrom", str(root / "avalanche/build/avalanche.iso"),
                       "-netdev", "user,id=net", "-device", adapter + ",netdev=net,romfile="]
            command.extend(["-drive", f"file={disk},format=raw," +
                            ("if=ide" if storage == "ATA" else "if=none,id=data")])
            command.extend(extra)
            for variable, option in [("QEMU_BIOS", "-bios"), ("QEMU_DATA", "-L")]:
                if os.environ.get(variable):
                    command.extend([option, os.environ[variable]])
            machine = subprocess.Popen(command, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            def prompt():
                output = bytearray()
                deadline = time.monotonic() + 60
                while b"avalanche> " not in output and time.monotonic() < deadline:
                    if select.select([machine.stdout], [], [], .2)[0]:
                        data = os.read(machine.stdout.fileno(), 65536)
                        if not data:
                            break
                        output.extend(data)
                assert b"avalanche> " in output, output.decode(errors="replace")
                assert b"KERNEL EXCEPTION" not in output, output
                return bytes(output)
            def run(text):
                machine.stdin.write((text + "\n").encode())
                machine.stdin.flush()
                return prompt()
            try:
                prompt()
                inventory = run("devices")
                assert (storage + " disk ").encode() in inventory, inventory
                if chipset == "q35":
                    assert b"PCI Express ECAM:" in inventory, inventory
                assert b"ext4" in run("fs"), inventory
                if not boot:
                    assert nic.encode() in run("net")
                    lease = run("dhcp")
                    assert b"IPv4 10.0.2.15" in lease, lease
                    reply = run("ping 10.0.2.2")
                    assert b"Reply in " in reply, reply
                    run("write transport-check PERSISTENT-" + storage)
                    assert b"Disks synchronized." in run("sync")
                    print(f"PASS {chipset}: {storage} write/flush, {nic} DHCP/ARP/ICMP, "
                          "8 GiB RAM", flush=True)
                contents = run("run cat transport-check" if boot else "cat transport-check")
                assert ("PERSISTENT-" + storage).encode() in contents, contents
                if boot:
                    assert b"Process exit: 0" in contents, contents
                    print(f"PASS {storage} ext4 data survives reboot and ring-3 file reads",
                          flush=True)
            finally:
                if machine.poll() is None:
                    machine.terminate()
                    machine.wait(timeout=5)
        check = subprocess.run(["e2fsck", "-fn", str(disk)], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        assert check.returncode == 0, check.stdout.decode(errors="replace")
        print(f"PASS {storage} host e2fsck clean", flush=True)
