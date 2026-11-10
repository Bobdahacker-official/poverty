# Magenta 0.4.0

This release supersedes the 0.3.0 download and includes the Magenta desktop ISO,
Avalanche console ISO, data images, kernel ELF, source archive, and checksums.

- Files adds Trash and Restore, secondary ext4 volumes, and USB hotplug and eject.
- Terminal adds bounded text pipelines and output redirection. The editor adds
  Save As, Find, Go to Line, and recovery drafts.
- The desktop adds a signed app library, opt-in session restore, and Downloads.
  NetSurf's Downloads integration requires rebuilding the NetSurf port.
- Audio adds HDA microphone capture, stereo HDMI/DisplayPort output, a limited
  USB Audio Class 1 playback mode, Classic Bluetooth pairing, and streaming
  pre-encoded SBC to a compatible A2DP sink.
- The RTL8811CU/RTL8821CU USB transport probe identifies compatible hardware.
  Direct Wi-Fi scanning and association are unavailable in this release.

The images were compiled and packaged. No runtime tests or VM boots were performed.
