# Magenta

**Magenta 0.1** is a small text-based distribution built on [Avalanche Kernel](avalanche/). Its interface is a native C userspace program with numbered menus and magenta headings, backed by kernel system calls.

The Magenta program, extended kernel, BIOS/UEFI ISO and writable data image have been compiled and packaged. **Magenta has not been booted or runtime-tested**, following the request to omit testing. Earlier Avalanche test results do not establish that these new images work at runtime.

## Downloads

- [Magenta BIOS/UEFI ISO](magenta/images/magenta.iso) — boot media with a default Magenta entry and a separate rescue-console entry.
- [Magenta writable data disk](magenta/images/magenta-data.img) — bundled applications and the `welcome` file in an Avalanche filesystem.
- [Distribution notes](magenta/images/NOTICE) — validation status, limits and licensing.

The ISO is read-only boot media. Attach the separate writable data disk for persistent files. Without a recognized disk, the interface uses RAM storage and identifies it as temporary.

## Interface

Choose a numbered option and press Enter:

| Screen | Implemented actions |
| --- | --- |
| Files | List files, read text or a bounded binary hex view, create text, replace or append text, delete with confirmation. |
| Programs | Launch bundled or disk-based ELF applications in the foreground or background and report foreground exit status. |
| Processes | Show task IDs and states, wait for child processes, terminate a selected task with confirmation. |
| Network | Show adapter/address information, request DHCP, apply static IPv4 settings, ping, resolve DNS, launch an HTTP client. |
| System | Show actual memory, storage and uptime information, list detected devices, read the firmware clock. |
| Shell | Open the userspace shell; `exit` returns to Magenta. |
| Power | Confirm restart, halt or exit to the rescue console. |

The text editor accepts ASCII text, up to 511 characters per entered line and 64 KiB per file. `.save` writes changes; `.cancel` discards them. The interface rejects binary files for editing. Network configuration lasts for the session.

## Implementation and builds

[Magenta's interface](avalanche/user/magenta.c) runs in ring 3. Hardware access, storage, process isolation and networking remain in the kernel. The default boot entry passes `--magenta`; the kernel then launches the interface. The Magenta executable is also embedded as a fallback when no data disk is attached.

`make -C magenta` builds the distribution under `avalanche/build/`. `make -C magenta package` publishes local build artifacts into `magenta/images/`, creating a fresh release data disk. Existing writable disks under `avalanche/build/` are preserved by ordinary builds.

Build prerequisites are GCC/binutils with freestanding i386 support, GNU Make, Python 3, GRUB BIOS and x86-64 EFI modules, xorriso and mtools. `GRUB_LIB` can select an alternative GRUB module directory.

## Scope

Avalanche supports up to eight isolated user processes and a limited static Linux i386 ABI subset. Supported storage includes legacy ATA, AHCI and legacy/transitional VirtIO; network drivers include RTL8139, E1000 and legacy/transitional VirtIO. The filesystem has 32 flat files, each limited to 64 KiB.

Magenta is an early development distribution. General Linux application compatibility, dynamic linking, USB, NVMe, Wi-Fi, HTTPS and a graphical desktop are not provided. See [Avalanche's capability notes](avalanche/images/NOTICE) for the underlying kernel's limits and earlier validation results.

## Repository

- `magenta/` — distribution boot configuration, welcome file, build entry point and packaged images.
- `avalanche/` — kernel, hardware drivers, userspace applications and build tools.
- `index.html` — drawing Inc, the separate browser drawing game.

Source is covered by [Avalanche's MIT license](avalanche/LICENSE). Bundled GRUB retains its [own licensing](avalanche/images/GRUB-COPYRIGHT), with corresponding source included in [grub-source](avalanche/images/grub-source/).
