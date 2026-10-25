# Magenta

Magenta is the distribution planned for **Avalanche Kernel**: a small, real operating system with a basic **text-based user interface**.

This README is the reference for building Magenta. The repository currently contains Avalanche Kernel 0.3.0 and its command-line applications; a separate Magenta distribution image and interface have not been implemented yet.

## What to build

Build a compact distribution that boots into a usable text interface. Users should be able to manage files, run programs, view processes, and configure supported networking without needing to understand kernel internals.

The interface should use a clear Magenta title, readable text, simple numbered menus, and short prompts. Magenta-colored headings can be used where the console supports them. Keep keyboard navigation straightforward and provide a way to return to the main menu or open the shell.

This is a text interface, not a graphical desktop. It does not need windows, icons, a mouse-driven desktop, or a browser frontend.

## Initial interface

The first version should provide these working screens:

- **Files:** list, read, create, edit, and delete files; identify whether storage is persistent or held in RAM.
- **Programs:** list available applications and launch them as foreground or background processes.
- **Processes:** show process IDs and states, wait for children, and terminate processes.
- **Network:** show the active adapter and addresses, configure DHCP or a static address, and expose supported connectivity tools.
- **System:** show the kernel version, detected devices, memory usage, and uptime, with reboot and halt controls.
- **Shell:** open the existing userspace shell and return cleanly when it exits.

Every displayed action must call a real implementation. Report unsupported operations and failures clearly. Do not add decorative controls, simulated progress, invented device information, or success messages for actions that did not complete.

## Distribution requirements

Magenta should have its own reproducible image target, boot entry, startup path, and bundled applications while reusing Avalanche's kernel and drivers. Booting that image should launch the Magenta interface automatically.

Ship the user interface as a userspace program. Keep hardware access, process isolation, system calls, and storage drivers in the kernel. Handle missing disks or network adapters gracefully and make destructive storage operations explicit.

Provide a bootable BIOS/UEFI ISO and a separate writable data image. The distribution must remain usable with the hardware Avalanche actually supports; do not advertise unimplemented hardware or general Linux application compatibility.

## Existing foundation

[Avalanche Kernel](avalanche/) currently supplies:

- Up to eight isolated user processes with timer-driven scheduling, background jobs, and process waiting and termination.
- A userspace shell, file utilities, networking clients, and native application interfaces.
- A persistent filesystem, with a volatile RAM fallback when no recognized disk is mounted.
- ATA, AHCI, and legacy VirtIO storage; RTL8139, E1000, and legacy VirtIO networking.
- A limited Linux i386 static-ELF and system-call compatibility layer.

The existing [kernel ISO](avalanche/images/avalanche.iso) and [data disk](avalanche/images/avalanche-data.img) are Avalanche development images, not Magenta releases. See the [capability and validation notes](avalanche/images/NOTICE) for exact limits and checks already performed.

## Completion criteria

Magenta's first release is complete when its own image boots into the text interface, the listed actions operate on real files and processes, supported networking can be configured, and saved files survive a restart when writable storage is attached. Verify the distribution's BIOS and UEFI boot paths and report the hardware configurations exercised.

## Repository contents

- `avalanche/` — kernel, drivers, application sources, build tools, and development images.
- `index.html` — drawing Inc, the separate browser drawing game previously created in this repository.

Avalanche source licensing and bundled bootloader notices are available in [LICENSE](avalanche/LICENSE) and [GRUB-COPYRIGHT](avalanche/images/GRUB-COPYRIGHT).
