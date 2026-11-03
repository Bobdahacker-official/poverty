# ClassiCube for Magenta

This port builds the BSD-3-Clause ClassiCube 1.3.9 C client with its upstream software rasteriser and a Magenta native window backend. It contains no Minecraft Java Edition code or assets.

```sh
make classicube
gzip -dk Images/classicube-data.img.gz
qemu-system-x86_64 -m 512 -boot d -cdrom Images/magenta.iso \
  -drive file=classicube-data.img,format=raw,if=ide \
  -nic user,model=e1000 -device virtio-rng-pci,disable-modern=on
```

Open `classicube.mgnta` from Files or run it in Terminal. The app starts its offline launcher, where local Classic worlds can be created and played. It uses the original upstream source, graphics pipeline, world generator, input system and launcher.

The build recipe pins the upstream 1.3.9 source archive by SHA-256, packages its license and credits on the ext4 app disk, and checks the finished disk with e2fsck.

Upstream source: <https://github.com/ClassiCube/ClassiCube>. ClassiCube is a standalone Minecraft Classic protocol client and is not affiliated with Mojang or Microsoft.
