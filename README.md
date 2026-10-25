# Magenta
*This OS is not a hobby OS, it is not "niche", and it is not educational or a proof of concept or portfolio project! please do no call it that. this is in the first stage of production.* 

**Magenta 0.1** is a small, text-based operating system distribution powered by [Avalanche Kernel](avalanche/). It includes a native userspace interface, bundled applications, a BIOS/UEFI boot image, and a separate writable data disk.

The interface uses numbered menus, keyboard input, and magenta headings. File operations, process management, networking, and system information use implemented kernel services.

The Magenta program, extended kernel, BIOS/UEFI ISO and writable data image have been compiled and packaged. **Magenta has not been booted or runtime-tested**, following the request to omit testing. Earlier Avalanche test results do not establish that these new images work at runtime.

## Downloads

- [Download Magenta BIOS/UEFI ISO](https://github.com/Bobdahacker-official/poverty/raw/refs/heads/main/magenta/images/magenta.iso) — boot media with a default Magenta entry and a separate rescue-console entry.
- [Download Magenta writable data disk](https://github.com/Bobdahacker-official/poverty/raw/refs/heads/main/magenta/images/magenta-data.img) — bundled applications and the `welcome` file in an Avalanche filesystem.
- [Distribution notes](magenta/images/NOTICE) — validation status, limits and licensing.

The ISO is read-only boot media. Attach the separate writable data disk for persistent files. Without a recognized disk, the interface uses RAM storage and identifies it as temporary.

## VirtualBox

Magenta's BIOS boot image and Intel E1000 network driver are configured for VirtualBox. Create a VM manually with 256 MiB of memory, attach `magenta.iso` as an IDE optical drive, attach `magenta-data.img` as a virtual hard disk, and select the Intel PRO/1000 MT Desktop (`82540EM`) network adapter with NAT.

The included [`copy.sh`](copy.sh) copies both release files to a directory:

```sh
./copy.sh /path/to/Magenta
```

It can also create and register a complete VirtualBox machine. This requires VirtualBox's `VBoxManage` command and converts a copy of the data image to VDI, keeping the packaged image unchanged:

```sh
./copy.sh --virtualbox Magenta
VBoxManage startvm Magenta
```

The data disk is where saved files live. Keep its generated `magenta-data.vdi` when upgrading the ISO; replacing it starts a fresh installation.

## Browser emulator (Copy.sh)

[Launch Magenta in Copy.sh](copy.sh.html) to boot the distribution in the browser through Copy.sh's v86 emulator. The page loads the project ISO and data image, uses the emulator's VirtIO network adapter, and needs no local installation. Its v86 runtime and BIOS files are included in this repository, so it does not depend on Copy.sh allowing external script requests. Click the display after it starts to direct keyboard input to Magenta.

The packaged data disk is read-only to the browser. Changes made in this emulator are retained only for its current browser session; use the VirtualBox data disk for persistent saved files.

## Interface

The default boot entry is configured to start Magenta automatically. Choose a numbered option and press Enter:

```text
MAGENTA 0.1 | Home

1  Files       Read, create, edit and delete
2  Programs    Run applications
3  Processes   View and manage tasks
4  Network     Configure and connect
5  System      Devices, memory and uptime
6  Shell       Open a command prompt
0  Power       Restart, halt or rescue

Magenta>
```

| Screen | Implemented actions |
| --- | --- |
| Files | List files, read text or a bounded binary hex view, create text, replace or append text, delete with confirmation. |
| Programs | Launch bundled or disk-based ELF applications in the foreground or background and report foreground exit status. |
| Processes | Show task IDs and states, wait for child processes, terminate a selected task with confirmation. |
| Network | Show adapter/address information, request DHCP, apply static IPv4 settings, ping, resolve DNS, launch an HTTP client. |
| System | Show actual memory, storage and uptime information, list detected devices, read the firmware clock. |
| Shell | Open the userspace shell; `exit` returns to Magenta. |
| Power | Confirm restart, halt or exit to the rescue console. |

The text editor accepts ASCII text, up to 511 characters per entered line and 64 KiB per file. `.save` writes changes; `.cancel` discards them. The interface rejects binary files for editing; its reader shows a bounded hexadecimal view instead. Prefix a command with another dot to insert it literally, such as `..save`. Network configuration lasts for the session.

## Bundled applications

| Program | Purpose |
| --- | --- |
| `magenta` | The text-based distribution interface. |
| `ush` | Interactive shell; type `help` for its commands. |
| `hello` | Display arguments and uptime ticks. |
| `cat` | Read a file supplied as an argument. |
| `udp` | Send a message to an IPv4 address and UDP port. |
| `http` | Fetch an HTTP resource on port 80. |
| `worker` | CPU-bound background application. |
| `linuxhello` | Exercise the supported Linux i386 file, memory and TLS APIs. |
| `linuxfork` | Demonstrate Linux-style fork, wait and private process memory. |

The data disk also contains a `welcome` text file. Foreground programs return to Magenta when they exit. Background jobs can be viewed in Processes; wait for completed children to reclaim their process slots.

## Implementation and builds

[Magenta's interface](avalanche/user/magenta.c) runs in ring 3. Hardware access, storage, process isolation and networking remain in the kernel. The default boot entry passes `--magenta`; the kernel then launches the interface. The Magenta executable is also embedded as a fallback when no data disk is attached.

Build the distribution:

```sh
make -C magenta
```

Outputs are `avalanche/build/magenta.iso` and `avalanche/build/magenta-data.img`. Ordinary builds retain an existing writable data image, including any files already saved in it.

Package fresh distribution artifacts:

```sh
make -C magenta package
```

This replaces the release artifacts in `magenta/images/` and creates a fresh release data disk from the bundled applications and welcome file. It does not refresh applications inside an existing working data disk.

Build prerequisites are GCC/binutils with freestanding i386 support, GNU Make, Python 3, GRUB BIOS and x86-64 EFI modules, xorriso and mtools. `GRUB_LIB` can select an alternative GRUB module directory.

## Scope

Avalanche supports up to eight isolated user processes and a limited static Linux i386 ABI subset. Supported storage includes legacy ATA, AHCI and legacy/transitional VirtIO; network drivers include RTL8139, E1000 and legacy/transitional VirtIO. The filesystem has 32 flat files, each limited to 64 KiB. The kernel is 32-bit x86; its UEFI loader targets x86-64 firmware. Secure Boot signatures are not included.

Magenta is an early development distribution. General Linux application compatibility, dynamic linking, USB, NVMe, Wi-Fi, HTTPS and a graphical desktop are not provided. See [Avalanche's capability notes](avalanche/images/NOTICE) for the underlying kernel's limits and earlier validation results.

## Repository

- `magenta/` — distribution boot configuration, welcome file, build entry point and packaged images.
- `avalanche/` — kernel, hardware drivers, userspace applications and build tools.
- `index.html` — drawing Inc, the separate browser drawing game.

Source is covered by [Avalanche's MIT license](avalanche/LICENSE). Bundled GRUB retains its [own licensing](avalanche/images/GRUB-COPYRIGHT), with corresponding source included in [grub-source](avalanche/images/grub-source/). The included Copy.sh v86 runtime and BIOS files retain their [BSD-2-Clause license](emulator/v86/LICENSE).
