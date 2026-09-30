# k1cam

Replacement firmware for the USB webcam module of the Creality K1 3D printer
(Ingenic T31L, GalaxyCore GC2083, 8 MiB SPI NOR). The camera enumerates as a
standard UVC webcam (so far tested with Linux hosts), with:

- MJPEG and H.264 at 1920x1080, 1280x960, 1280x720, 800x600, 640x480 and
  640x360, 5 to 30 fps;
- the usual image controls (brightness, contrast, white balance, exposure,
  zoom and pan/tilt, ...) as standard UVC controls, and the ISP and encoder
  settings that UVC has no control for on a vendor extension unit, reachable
  with `uvcdctl` (package/k1cam-uvcd/uvcdctl);
- settings that persist across reboots, with a factory reset;
- a shell on the USB serial (ACM) port.

The camera only runs its image pipeline while a program is streaming.

## Build

Needs a Linux x86_64 host with Buildroot's usual prerequisites. The kernel
sources are fetched automatically: the thingino-linux fork's commit pinned in
the defconfig, from its `k1cam` branch.

```sh
git submodule update --init
make
```

The result is `output/k1cam/images/k1cam.bin`, a full image of the 8 MiB
flash:

| partition | offset   | size     |                                  |
| --------- | -------- | -------- | -------------------------------- |
| boot      | 0x000000 | 320 KiB  | U-Boot                           |
| env       | 0x050000 | 64 KiB   | U-Boot environment               |
| backup    | 0x060000 | 64 KiB   |                                  |
| kernel    | 0x070000 | 1600 KiB | Linux 3.10 uImage                |
| rootfs    | 0x200000 | 2560 KiB | squashfs                         |
| data      | 0x480000 | 3584 KiB | jffs2 overlay (settings)         |

The layout is defined once, in `board/k1cam/uenv.txt`. See AGENTS.md for how
the tree is organised.

## Origins and licences

k1cam started from [thingino](https://github.com/themactep/thingino-firmware)'s
firmware tree and retains a few upstream camera packages under their original
names, under the MIT licence in LICENSE. Its board configuration, rootfs and
runtime are K1-specific. k1cam is not affiliated with the thingino project.

Also used and fetched by the build:

- the Linux kernel from
  [itzexor/thingino-linux](https://github.com/itzexor/thingino-linux)
  (GPL-2.0), branch `k1cam`, with k1cam's USB gadget changes;
- Ingenic's ISP, encoder and sensor drivers and libraries, via thingino's
  and gtxaspec's repositories (ingenic-sdk, ingenic-lib, raptor-hal);
- [Buildroot](https://buildroot.org) (GPL-2.0) and U-Boot (GPL-2.0).

uvcd and uvcdctl (package/k1cam-uvcd) are GPL-3.0.
