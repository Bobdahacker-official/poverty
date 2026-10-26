# Magenta 0.2.0 — Mountains

This release adds the Mountains wallpaper, files and folders on ext4, desktop shortcuts, and a registry editor.

## What's new

- The desktop boots at 1920×1080. Text, windows, controls, and the dock scale together.
- Mountains is the default wallpaper, with smooth ridgelines and a river that flows to the bottom edge.
- Files can create files and folders, rename them, and delete files and empty folders on ext4.
- Right-click an app to add a desktop shortcut. Right-click the shortcut to remove it.
- Settings has a Registry editor for bar visibility, shortcut choices, and positions. Terminal commands edit the same values.
- The browser viewer fits the screen and has a keyboard toolbar for phones and tablets.

Terminal, Files, Text editor, Tasks, Network, Packages, and Settings are included. Magenta runs on Avalanche Kernel, written in C and assembly, with BIOS and UEFI boot support.

## Files to download

- **magenta.iso**: Magenta boot image.
- **avalanche.iso**: Avalanche Kernel and console.
- **magenta-data.img** and **avalanche-data.img**: writable Avalanche disks with the bundled programs.
- **avalanche.elf**: the kernel executable.
- **magenta-source.zip**: source and license files.
- **magenta-desktop.png**, **magenta-launcher.png**, and **magenta-files.png**: desktop screenshots.
- **SHA256SUMS**: download checksums.

For a VM, attach the ISO as an optical drive and the matching data image as a hard disk. The Codespaces launcher creates a separate ext4 disk on first start and keeps it across restarts.

## Checks

Both boot images were rebuilt. The screenshots come from Magenta running at 1920×1080. A folder created in Files was checked with debugfs, and the ext4 disk passed e2fsck.
