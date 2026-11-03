# NetSurf for Magenta

NetSurf 3.11 runs as a ring-3 x86-64 application on Avalanche Kernel. Its libnsfb surface sends pixel updates and receives keyboard, pointer and window-close events through Magenta's GUI API. NetSurf uses its HTML, CSS and JavaScript engines, FreeType text rendering, static libcurl/OpenSSL transport and a Mozilla CA bundle.

Build the kernel and app disk:

```sh
make package
make netsurf
gzip -dk Images/netsurf-data.img.gz
qemu-system-x86_64 -m 512 -boot d -cdrom Images/magenta.iso \
  -drive file=Images/netsurf-data.img,format=raw,if=ide \
  -nic user,model=e1000 -device virtio-rng-pci,disable-modern=on
```

Open `netsurf.mgnta` in Files or run it in Terminal:

```sh
netsurf.mgnta
netsurf.mgnta https://www.netsurf-browser.org/
```

The disk includes Magenta's bundled applications, browser resources, fonts, certificates and component licenses. NetSurf opens its local welcome page by default. Its address bar accepts local files and HTTP/HTTPS URLs. `-w` and `-h` select the browser surface dimensions; the desktop fits the surface within the window.

The build downloads checksum-pinned NetSurf, curl, libiconv, FreeType and CA sources. Install a C toolchain, binutils, Make, Python 3, curl, patch, pkg-config, flex, bison, gperf, m4, OpenSSL development files, zlib, libpng, libjpeg, DejaVu fonts and e2fsprogs. Kernel image builds also use GRUB, xorriso and mtools. `python3 ports/netsurf/build.py --help` lists cache, font-directory and job options. Source patches and the native surface are kept here; downloaded source trees stay in the build cache.

The resolver uses Avalanche's DNS service. The socket bridge carries TCP through the kernel's network stack. Fetches run one at a time, matching the current TCP connection ownership model. Certificate checks use OpenSSL's DNS or IP subjectAltName verification and the disk's CA bundle.

```sh
make test-netsurf
```

The QEMU checks use local HTTP and TLS servers, an ephemeral test CA and screenshot pixel checks. They exercise HTML/CSS, JavaScript DOM changes, pointer navigation, HTTPS validation, untrusted-certificate rejection, native SDK drawing and process cleanup. The test CA is added only to a temporary test disk.

NetSurf source and licenses: <https://www.netsurf-browser.org/>. The native surface and resolver are supplied under the repository's license. Component license files are included under `/netsurf/licenses` on the app disk.
