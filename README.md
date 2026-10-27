# k1cam

Replacement firmware for the USB webcam module of the Creality K1 3D printer
(Ingenic T31L, GalaxyCore GC2083, 8 MiB SPI NOR). The camera enumerates as a
standard UVC webcam (so far tested with Linux hosts), with:

- MJPEG and H.264 at 1920x1080, 1280x960, 1280x720, 800x600, 640x480 and
  640x360, 5 to 30 fps;
- the usual image controls (brightness, contrast, white balance, exposure,
  zoom and pan/tilt, ...) as standard UVC controls, and the ISP and encoder
  settings that UVC has no control for on a vendor extension unit, reachable
  with [uvcdctl](package/k1cam-uvcd/uvcdctl/);
- settings that persist across reboots, with a factory reset;
- flash dumps and A/B firmware updates over the same USB device (`uvcdctl
  firmware`), which fall back to the previous firmware by themselves if the
  new one does not work (see [firmware updates](FIRMWARE-UPDATE.md));
- a shell on the USB serial (ACM) port.

The sensor and ISP run from boot on the default 1080p view, so exposure
and white balance are already settled when a program starts streaming.
With nothing streaming they run at 30 fps until exposure settles, then at
5 fps until the light changes. Capture and encoding only run while a
program is streaming.

## Projects in the ecosystem

| Project | Role |
| --- | --- |
| [k1cam-firmware](https://github.com/itzexor/k1cam-firmware) (this repository) | Buildroot external tree for the camera firmware: board configuration, the `uvcd` camera daemon, persistent settings, and firmware image/update packaging. |
| [thingino-linux](https://github.com/itzexor/thingino-linux/tree/k1cam) | Linux kernel fork, with the K1 USB webcam gadget, UVC controls, extension unit and USB serial port. The firmware build fetches the exact commit pinned in `configs/k1cam_defconfig`. |
| [k1cam-flasher](https://github.com/itzexor/k1cam-flasher) | Desktop GUI for full flash backups and initial installation/recovery through the Ingenic USB boot ROM, plus A/B updates over the running k1cam camera's UVC extension unit. See its README for platform support and USB driver setup. |
| [uvcdctl](https://github.com/itzexor/k1cam-firmware/tree/k1cam/package/k1cam-uvcd/uvcdctl) | Linux CLI in this repository for image/encoder controls, flash backups and A/B updates over USB. It builds independently on the host and is also included in the camera firmware. |

Build this firmware, or download the newest build of the `k1cam` branch from
the [nightly release](https://github.com/itzexor/k1cam-firmware/releases/tag/nightly)
([`k1cam.bin`](https://github.com/itzexor/k1cam-firmware/releases/download/nightly/k1cam.bin),
[`k1cam.upd`](https://github.com/itzexor/k1cam-firmware/releases/download/nightly/k1cam.upd),
replaced on every push), to get `k1cam.bin` and `k1cam.upd`. Use
[k1cam-flasher's installation instructions](https://github.com/itzexor/k1cam-flasher#using-it)
to install the full `k1cam.bin` image through the boot ROM; this also installs
the A/B flash layout and resets settings. Once the camera runs k1cam on that
layout, use `uvcdctl firmware upgrade` or the flasher's A/B Update tab with
`k1cam.upd` for subsequent updates over its normal USB connection. The
[firmware update guide](FIRMWARE-UPDATE.md) covers layout migration, backups
and rollback.

The running firmware uses `f055:4b31` (unofficial FOSS community VID,
`4b31` = "K1"), with manufacturer `k1cam` and product `K1 Camera`. See
[USB identity](USB-IDENTITY.md) for the collision checks and migration from
the earlier `1d6b:0110` identity.

## Build the firmware

Needs a Linux x86_64 host with Buildroot's usual prerequisites. The kernel
sources are fetched automatically from
[itzexor/thingino-linux](https://github.com/itzexor/thingino-linux/tree/k1cam):
the commit pinned in the defconfig, from its `k1cam` branch. No sibling kernel
checkout is needed.

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
with `k1cam.bin` once ([firmware update guide](FIRMWARE-UPDATE.md)). See
[AGENTS.md](AGENTS.md) for how the tree is organised.

## Get uvcdctl for a Linux host

The [nightly release](https://github.com/itzexor/k1cam-firmware/releases/tag/nightly)
includes static `uvcdctl` builds with SHA-256 checksums:

| Host architecture (`uname -m`) | Download |
| --- | --- |
| `x86_64` | [uvcdctl-linux-x86_64.tar.gz](https://github.com/itzexor/k1cam-firmware/releases/download/nightly/uvcdctl-linux-x86_64.tar.gz) |
| `aarch64` (ARM64) | [uvcdctl-linux-aarch64.tar.gz](https://github.com/itzexor/k1cam-firmware/releases/download/nightly/uvcdctl-linux-aarch64.tar.gz) |
| `armv7l` (32-bit hard-float ARM) | [uvcdctl-linux-armv7.tar.gz](https://github.com/itzexor/k1cam-firmware/releases/download/nightly/uvcdctl-linux-armv7.tar.gz) |
| `mips` (32-bit little-endian MIPS, mipsel) | [uvcdctl-linux-mipsel.tar.gz](https://github.com/itzexor/k1cam-firmware/releases/download/nightly/uvcdctl-linux-mipsel.tar.gz) |

Download the archive for your Linux host and extract it, for example:

```sh
tar -xzf uvcdctl-linux-x86_64.tar.gz
./uvcdctl help
```

The MIPS build requires little-endian hardware; `uname -m` does not identify
the byte order.

The binary needs no installed runtime libraries. CI builds all four host
architectures independently of the firmware and runs their help commands
(under QEMU for ARM and MIPS). `uvcdctl` uses Linux's V4L2/UVC interfaces;
macOS and Windows are not supported.

## Build uvcdctl on a Linux host

To control an already-flashed camera, only `uvcdctl` is needed. Its standalone
build requires `make`, a C11 compiler, libc development files and Linux
userspace headers (V4L2/UVC). It uses libc and the kernel's `uvcvideo` driver;
no libusb, camera SDK, Buildroot build or submodule initialization is needed.
Keep the firmware checkout intact: the CLI also compiles shared firmware
bundle code from its sibling `../src` directory.

From a fresh checkout:

```sh
git clone --branch k1cam https://github.com/itzexor/k1cam-firmware.git
cd k1cam-firmware/package/k1cam-uvcd/uvcdctl
make
./uvcdctl help
```

The executable is `uvcdctl` in that directory. Run it there as `./uvcdctl`,
or optionally install it to `/usr/bin/uvcdctl` with `sudo make install`.
This builds for the machine running `make`, independently of the MIPS camera
firmware build.

## Use uvcdctl

Connect the camera running k1cam firmware over USB. The host user needs read
and write access to its `/dev/videoN` node (commonly through the `video`
group or desktop device permissions). `uvcdctl` finds the camera by probing
its UVC extension unit; use `--device /dev/videoN` before the command to
select one explicitly when several cameras are connected.

These examples use the installed command; use `./uvcdctl` from the build
directory if it is not installed:

```sh
uvcdctl list                              # values, ranges and defaults
uvcdctl describe h264-bitrate-kbps         # explain a control
uvcdctl get spatial-denoise
uvcdctl set h264-bitrate-kbps 6000
uvcdctl --device /dev/video0 set rotation 180
uvcdctl firmware info                     # build, layout and boot slots
uvcdctl firmware dump k1-backup.bin        # verified full flash backup
uvcdctl firmware upgrade /path/to/k1cam.upd
uvcdctl help firmware
```

Control changes persist automatically on the camera; there is no save step.
On the camera itself, the included `uvcdctl` uses the daemon's local socket
for controls. On the USB host, it uses V4L2 and UVC extension unit requests.
See the [uvcdctl README](package/k1cam-uvcd/uvcdctl/README.md) for the full
command reference, device selection, reset behavior and firmware exit codes.

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
