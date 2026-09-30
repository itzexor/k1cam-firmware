# k1cam

Firmware for the USB webcam module of the Creality K1 3D printer (Ingenic
T31L, GalaxyCore GC2083, 8 MiB SPI NOR, powered and connected only over USB).
It is a Buildroot external tree that builds exactly this one camera.

k1cam grew out of thingino's firmware tree and is not affiliated with
thingino. Components kept from thingino keep their names (thingino-*,
ingenic-*, the thingino-linux kernel repo, overlay/, scripts/) and the MIT
notice in LICENSE. Work written for k1cam is named k1cam (package/k1cam-uvcd,
board/k1cam, the USB gadget identity). Never rename a kept thingino
component; never give k1cam's own work a thingino name.

## Build

```sh
make                    # configure on first use, build output/k1cam/images/k1cam.bin
make menuconfig         # then `make savedefconfig` to keep the change
make savedefconfig      # writes configs/k1cam_defconfig
make k1cam-uvcd-rebuild # any other target goes to Buildroot
make clean              # removes output/k1cam; downloads in dl/ stay
make update             # Buildroot submodule + package/all-patches/buildroot
```

A configuration change does not rebuild what is already built (plain
Buildroot behaviour). After changing the defconfig, a kernel pin or a package
name, run `make clean all`. Keep the full log of every build (redirect it to a
file) and read it, rather than trusting a short tail.

## Layout

```
Makefile                 thin wrapper around buildroot/ (submodule)
configs/k1cam_defconfig  the whole configuration (savedefconfig output)
k1cam.mk                 values thingino's packages read from their build
                         environment (SoC, sensor, ISP clock, board name)
board/k1cam/uenv.txt     U-Boot environment, and with it the flash layout
                         (mtdparts); also U-Boot's built-in default
board/k1cam/post-build.sh   trims the rootfs to a USB webcam, k1cam identity
board/k1cam/post-image.sh   packs k1cam.bin from the layout in uenv.txt
package/k1cam-uvcd/      uvcd (UVC gadget daemon), uvcdctl, S31uvcd, tests
package/thingino-*, ingenic-*   kept from thingino
overlay/, scripts/       thingino's rootfs overlay and post-build script
```

The flash layout is fixed. To change it, edit mtdparts and the matching
kern_*/data_* lines in board/k1cam/uenv.txt; post-image.sh refuses an image
whose layout, addresses or sizes disagree.

## Kernel

The kernel is `../thingino-linux` (branch `k1cam`), pinned by SHA in the
defconfig. Commit kernel work there first, then bump
`BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION`; uncommitted kernel changes never
reach an image. The USB gadget descriptors live in
`drivers/usb/gadget/webcam.c`.

## uvcd tests

Host tests (config, gadget, pipeline, startup) need the raptor-hal headers
from a build: `package/k1cam-uvcd/tests/run.sh` (after `make`). The
supervisor test is `python3 package/k1cam-uvcd/tests/supervisor_test.py`.

## Working rules

- Never flash the camera; the user does. Uploading test files to it,
  restarting services and rebooting it for testing are fine.
- Never delete files irreversibly: a file leaves the tree through a commit,
  so git history can bring it back.
- Commits: `git commit -s`, the body says why, keep the attribution lines.
- Shell scripts are POSIX `/bin/sh`, ASCII only, formatted with
  `shfmt -i 0 -ci` (the pre-commit hook in .githooks does it; enable with
  `git config core.hooksPath .githooks`).
- Stay inside this directory and ../thingino-linux; ask before searching
  anywhere else.
