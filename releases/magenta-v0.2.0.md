# Magenta 0.2.0 — Mountains

Magenta is a native x86-64 desktop distribution powered by Avalanche Kernel, written in C and assembly. This release brings a polished desktop, persistent folders, and configurable desktop shortcuts.

## Highlights

- Full HD boot framebuffer with native scaling for application windows, text, icons, the dock, and the top panel.
- Mountains wallpaper with continuous ridgelines and a flowing river, enabled by default.
- Files supports creating files and folders, renaming items, and deleting files and empty folders on ext4.
- Right-click applications in the launcher or dock to add desktop shortcuts; remove them from the shortcut menu.
- Persistent Registry editor in Settings, with matching Terminal commands for desktop shortcuts, bar visibility, and positions.
- Terminal, Files, Text editor, Tasks, Network, Packages, and Settings are included.
- Browser desktop through QEMU and noVNC, with mobile keyboard controls and responsive viewport fitting.
- BIOS and UEFI boot support, persistent ext4 storage, and VirtIO networking in the browser launcher.

## Downloads

- **magenta.iso** — Magenta boot image for BIOS and UEFI virtual machines.
- **avalanche.iso** — Avalanche Kernel boot image and console.
- **magenta-data.img** and **avalanche-data.img** — writable Avalanche filesystem disks with bundled applications.
- **avalanche.elf** — kernel executable.
- **magenta-source.zip** — source and included third-party license files for this release.
- **magenta-desktop.png**, **magenta-launcher.png**, and **magenta-files.png** — captures of the running desktop.
- **SHA256SUMS** — checksums for the downloadable artifacts.

Attach the ISO as an optical drive and the matching writable data image as a hard disk in an x86-64 virtual machine. The Codespaces launcher creates its persistent ext4 disk on first start and keeps it across restarts.

## Validation

Both boot images were rebuilt. The desktop was captured running at 1920×1080. A directory created through Files was inspected with host debugfs, and the resulting ext4 disk passed e2fsck.

Future releases are published for explicitly tagged version updates. Ordinary commits do not produce releases.
