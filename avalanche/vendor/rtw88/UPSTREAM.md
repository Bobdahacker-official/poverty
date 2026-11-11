# RTL8811CU / RTL8821CU hardware bootstrap

The power, efuse, reserved-page transport, DDMA download, CPU reset and
coexistence-register sequences in `src/rtw8821cu_hw.c` are adapted from
[rtw88](https://github.com/lwfinger/rtw88/tree/f1dd7c3a5b87478ab23c3dbb1f8f0d6b82f3f71c),
commit `f1dd7c3a5b87478ab23c3dbb1f8f0d6b82f3f71c` (2026-10-24):
`rtw8821c.c`, `rtw8821c.h`, `efuse.c`, `mac.c`, `mac.h`, `reg.h`,
`fw.c`, `fw.h`, `tx.h`, `usb.c` and `util.c`.
Those files are dual GPL-2.0 OR BSD-3-Clause; this adaptation uses BSD-3-Clause.
The Realtek copyright notice and BSD terms accompany the adaptation.

`rtw8821c_fw.bin` is the unmodified firmware image from that repository's
`firmware` directory. Its separate redistribution terms are in
`LICENSE.firmware`, obtained from linux-firmware's
`LICENCE.rtlwifi_firmware.txt`. Firmware is linked into the kernel unchanged.
The image's SHA-256 is
`2ef409bc418549fcf294061dd0cae1fc22fd9da79b60524950b25de18732f3f0`.

The bootstrap reads the USB factory MAC, board options, power calibration
map and channel-plan data. It does not program efuses. Delays use PIT-calibrated
TSC timing. USB control and bulk errors propagate; all register polls have
deadlines. Reserved-page registers are restored after each transfer, and
initialization failures attempt to turn the adapter off. USB detach drops
state without sending requests to a removed device.

`wifi init` explicitly requests power-up and firmware loading; enumeration
only reads the silicon ID. `wifi status` and `netdiag` report the last stage,
factory MAC and failing register. `wifi off` requests the USB power-off sequence.
This bootstrap does not initialize RF/baseband tables, calibrate the radio,
scan channels, associate with an access point, perform WPA2, or expose a
wireless Ethernet interface. Those station operations remain required for
end-to-end Wi-Fi. Firmware-running status never means connected.
