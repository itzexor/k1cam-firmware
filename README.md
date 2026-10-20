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
- flash dumps and A/B firmware updates over the same USB device (`uvcdctl
  firmware`), which fall back to the previous firmware by themselves if the
  new one does not work (see FIRMWARE-UPDATE.md);
- a shell on the USB serial (ACM) port.

The sensor and ISP run from boot on the default 1080p view, so exposure
and white balance are already settled when a program starts streaming.
With nothing streaming they run at 30 fps until exposure settles, then at
5 fps until the light changes. Capture and encoding only run while a
program is streaming.

## Build

Needs a Linux x86_64 host with Buildroot's usual prerequisites. The kernel
sources are fetched automatically: the thingino-linux fork's commit pinned in
the defconfig, from its `k1cam` branch.

```sh
git submodule update --init
make
```

The results are in `output/k1cam/images/`: `k1cam.bin`, a full image of the
8 MiB flash for installing (it resets the settings), and `k1cam.upd`, the
update bundle for `uvcdctl firmware upgrade`:

| partition | offset   | size     |                                         |
| --------- | -------- | -------- | --------------------------------------- |
| boot      | 0x000000 | 320 KiB  | U-Boot                                  |
| env_a     | 0x050000 | 64 KiB   | U-Boot environment                      |
| env_b     | 0x060000 | 64 KiB   | U-Boot environment, redundant copy      |
| kernel_a  | 0x070000 | 1280 KiB | slot a: Linux 3.10 uImage               |
| rootfs_a  | 0x1b0000 | 1792 KiB | slot a: squashfs                        |
| kernel_b  | 0x370000 | 1280 KiB | slot b: Linux 3.10 uImage               |
| rootfs_b  | 0x4b0000 | 1792 KiB | slot b: squashfs                        |
| data      | 0x670000 | 1600 KiB | jffs2: each slot's settings layer, /opt |

The layout is defined in `board/k1cam/uenv.txt` and, for the updater, in
`package/k1cam-uvcd/src/fw_layout.h`; the build checks that they agree.
Cameras on the earlier single-slot layout move to this one by being flashed
with `k1cam.bin` once (FIRMWARE-UPDATE.md). See AGENTS.md for how the tree is
organised.

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
