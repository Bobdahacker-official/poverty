# Realtek PCIe Ethernet

Hardware initialization comes from OpenBSD's `rge` driver:

- [if_rge.c](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_rge.c), revision 1.49, 2026-11-05.
- [if_rgereg.h](https://github.com/openbsd/src/blob/master/sys/dev/pci/if_rgereg.h), revision 1.21, 2026-11-05.
- [rgemcode.h](https://github.com/openbsd/src/blob/master/sys/dev/microcode/rge/rgemcode.h), revision 1.2, 2026-10-27.
- [Firmware license](https://github.com/openbsd/src/blob/master/sys/dev/microcode/rge/rge-license).

`hardware.inc` contains the MAC, PHY, ePHY, OCP, CSI, firmware upload, reset, and power sequences. `registers.h` retains the corresponding register definitions and descriptor layouts. `mac-setup.inc` adapts the single-queue MAC configuration. The original ISC notices accompany the code. PHY/CSI transaction timeouts propagate to the native initialization path, which disables DMA on failure.

`firmware.h` retains Realtek's firmware tables and copyright notice, with immutable, explicitly typed register pairs. `LICENSE.firmware` supplies the accompanying firmware distribution terms. Firmware travels with the kernel and does not depend on filesystem startup.

Avalanche supplies PCI discovery, BAR mapping, calibrated microsecond delays, DMA buffers, ownership barriers, single-queue polling, frame validation, and the TCP/IP interface in `src/rge.c`. RTL8125 A/B/D and RTL8126 use descriptor version 1; RTL8127 and RTL8127ATF use version 2. MSI-X and checksum, segmentation, VLAN, and pause offloads are disabled. Unknown MAC revisions fail initialization.

The upstream sources reviewed for wireless devices were Linux's [rtw89](https://github.com/torvalds/linux/tree/master/drivers/net/wireless/realtek/rtw89) and [btrtl](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btrtl.c). They depend on Linux's wireless/Bluetooth host stacks, firmware interfaces, bus APIs, and GPL-licensed kernel code. They are not part of this Ethernet port.
