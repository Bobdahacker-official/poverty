# Prebuilt image validation — 2026-10-27

Built from source commit `ba6f5d4a02f3532828b4c2a4cdfee7fb9168fc48`, with
the GRUB framebuffer configuration fix included in this image update.

## Build

- Debian WSL, GCC 14.2, binutils, GRUB 2.12, xorriso and mtools.
- `make -j1 -C avalanche iso magenta`, with a 1 GiB virtual-memory limit
  per build process.
- First-party code compiled with the existing `-Wall -Wextra -Werror` flags.
- The bundled Elk source emitted its existing GCC dangling-pointer warning;
  its build rule does not treat vendor warnings as errors.
- Rebuilt both boot ISOs and the checked-in `avalanche.elf`.
- Existing writable data images remain compatible and were not replaced.

## Headless VirtualBox checks

VirtualBox 7.2.6, BIOS boot, one virtual CPU, 512 MiB guest RAM, 16 MiB video
RAM, and a disposable 256 MiB ext4 test disk. Every start used
`VBoxManage startvm ... --type headless`. No graphical VM window was opened.

- Magenta reached `Magenta graphical desktop ready` in serial output.
- System Monitor displayed its live summary, kernel row and process controls.
- Files displayed the ext4 root and its actual directory entries.
- Browser accepted new-tab, switch-tab and close-tab keyboard shortcuts.
- Avalanche reached its rescue prompt and answered `version` and `mem`.
- The test VM was powered off after the checks.

The initial boot selected 640×480, clipping desktop application controls.
GRUB's multiboot loader overwrites `gfxpayload` from the kernel's framebuffer
header, so setting it only before `multiboot2` did not fix the resolution.
The menu entries now set it after loading the kernel, using an ordered list
of resolutions. The tested VirtualBox machine selected 1024×768.

These are BIOS boot and UI smoke checks, not exhaustive filesystem, browser
JavaScript, UEFI, or physical-hardware validation.

Verify the boot image checksums from the Images directory:

```sh
cd Images
sha256sum -c SHA256SUMS
```
