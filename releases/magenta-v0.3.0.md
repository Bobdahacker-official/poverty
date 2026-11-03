# Magenta Plus 0.3.0

Magenta Plus brings native graphical applications and a downloadable ClassiCube app disk to Magenta.

## Highlights

- Native userspace windows now have direct pixel surfaces, keyboard and pointer events, focus handling, desktop controls, Task Monitor integration, and process cleanup on close.
- NetSurf runs in its own Magenta window with HTML, CSS, JavaScript, TrueType text, HTTP, HTTPS, and certificate validation.
- ClassiCube 1.3.9 is included as a separate ext4 app disk. It uses its upstream C software renderer, launcher, local world generator, and Magenta input backend.
- The SDK includes native window APIs for graphical C applications and packaging tools for signed `.mgnta` executables.
- Package checksums cover every release download.

## Downloads

- **magenta.iso** — Magenta Plus boot image for BIOS and UEFI systems.
- **magenta-data.img** — writable Magenta disk with the bundled desktop applications.
- **classicube-data.img.gz** — writable ClassiCube app disk; extract it before attaching it to a VM.
- **avalanche.iso**, **avalanche-data.img**, and **avalanche.elf** — Avalanche Kernel console image, disk, and executable.
- **magenta-source.zip** — source code and license files for this release.
- **SHA256SUMS** — SHA-256 checksums for all release assets.

Boot `magenta.iso` with `magenta-data.img` as its IDE disk. To run ClassiCube, attach the extracted `classicube-data.img` as the writable IDE disk and open `classicube.mgnta` from Files or Terminal.

## Checks

The ClassiCube build was booted in QEMU. Its software-rendered launcher opened in a native Magenta window, accepted keyboard input, returned to Terminal when closed, and left its ext4 app disk clean under e2fsck.
