# k1cam

Firmware for the USB webcam module of the Creality K1 3D printer (Ingenic
T31L, GalaxyCore GC2083, 8 MiB SPI NOR, powered and connected only over USB).
It is a Buildroot external tree that builds exactly this one camera.

k1cam grew out of thingino's firmware tree and is not affiliated with
thingino. Upstream components keep their names (thingino-*, ingenic-* and
the thingino-linux kernel repo) and the MIT notice in LICENSE. Work written
for k1cam is named k1cam (package/k1cam-uvcd, board/k1cam, the USB gadget
identity). Never rename an upstream component; never give k1cam's own work a
thingino name.

## Build

```sh
make                    # configure on first use, build output/k1cam/images/k1cam.bin
make menuconfig         # then `make savedefconfig` to keep the change
make savedefconfig      # writes configs/k1cam_defconfig
make k1cam-uvcd-rebuild # any other target goes to Buildroot
make clean              # removes output/k1cam; downloads in dl/ stay
make update             # initialize/update the Buildroot submodule
```

A configuration change does not rebuild what is already built (plain
Buildroot behaviour). After changing the defconfig, a kernel pin or a package
name, run `make clean all`. Keep the full log of every build (redirect it to a
file) and read it, rather than trusting a short tail.

## Layout

```
Makefile                 thin wrapper around buildroot/ (submodule)
configs/k1cam_defconfig  the whole configuration (savedefconfig output)
board/k1cam/linux.config fixed T31L/GC2083 kernel configuration
board/k1cam/uboot.config fixed T31L SPI-NOR U-Boot configuration
board/k1cam/uenv.txt     U-Boot environment, flash layout (mtdparts), A/B boot logic
board/k1cam/rootfs-overlay/  fixed minimal runtime files and init scripts
board/k1cam/post-build.sh   target compatibility links and k1cam identity
board/k1cam/post-image.sh   packs k1cam.bin and k1cam.upd, checks the layout
board/k1cam/tests/       boot-test.sh: the boot logic in U-Boot's sandbox;
                         image-test.sh: post-image.sh's images and refusals
package/k1cam-uvcd/      uvcd (UVC gadget daemon), uvcdctl, k1cam-mkupd,
                         S31uvcd, tests
package/thingino-*, ingenic-*   upstream camera libraries and drivers
```

The flash layout is fixed: A/B slots, described in FIRMWARE-UPDATE.md. It is
in board/k1cam/uenv.txt (mtdparts and the matching kernel_*, rootfs_*_dev,
data_* lines) and in package/k1cam-uvcd/src/fw_layout.h, which the updater
takes every flash address from; post-image.sh refuses an image when they
disagree. A layout change also changes k1_layout and fw_env.config, and can
only reach a camera by flashing k1cam.bin.

## Kernel

The kernel is the `itzexor/thingino-linux` commit that the defconfig pins
(`BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION`), from its `k1cam` branch. Commit
kernel work there, push it, bump the pin to the new SHA and `make clean all`;
uncommitted or unpushed kernel changes never reach an image. The USB gadget
descriptors live in `drivers/usb/gadget/webcam.c`.

## uvcd tests

Host tests (config, firmware, protocol, gadget, pipeline, startup, uvcdctl)
need the raptor-hal headers from a build: `package/k1cam-uvcd/tests/run.sh`
(after `make`). The supervisor test is
`python3 package/k1cam-uvcd/tests/supervisor_test.py`. The U-Boot boot logic
is tested by `board/k1cam/tests/boot-test.sh`, which builds U-Boot's sandbox
into output/uboot-sandbox the first time, and the image packer by
`board/k1cam/tests/image-test.sh` (both after `make`).

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
