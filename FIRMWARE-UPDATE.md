# Firmware dump and A/B update over USB

The camera can be backed up and updated over the same UVC device that carries
its video: `uvcdctl firmware ...` on the host talks to uvcd through three
controls on Extension Unit 4. Updates go to the slot that is not running,
boot it as a trial, and keep it only once it has shown it works; otherwise
the camera returns to the previous slot by itself.

This needs the A/B flash layout, which a camera only gets by being flashed
with a full `k1cam.bin` once (see [Installing the A/B layout](#installing-the-ab-layout)).
Dumps also work on the original single-slot layout, once its firmware has
the protocol (a kernel with selectors 26-28 and a matching uvcd).

## Flash layout

| Partition | Offset | Size | Holds | Written by an update |
| --- | ---: | ---: | --- | --- |
| boot | 0x000000 | 320 KiB | U-Boot | never |
| env_a | 0x050000 | 64 KiB | U-Boot environment, copy 1 | slot variables only |
| env_b | 0x060000 | 64 KiB | U-Boot environment, copy 2 | slot variables only |
| kernel_a | 0x070000 | 1280 KiB | slot a kernel (uImage) | when a is inactive |
| rootfs_a | 0x1b0000 | 1792 KiB | slot a rootfs (SquashFS) | when a is inactive |
| kernel_b | 0x370000 | 1280 KiB | slot b kernel | when b is inactive |
| rootfs_b | 0x4b0000 | 1792 KiB | slot b rootfs | when b is inactive |
| data | 0x670000 | 1600 KiB | jffs2: `/opt` and each slot's settings layer | the new slot's layer only |
| all | 0x000000 | 8192 KiB | the whole flash (dumps) | never |

It is written down twice and the build refuses to pack an image if the two
disagree: `mtdparts` and the address variables in `board/k1cam/uenv.txt`
(U-Boot and Linux), and `package/k1cam-uvcd/src/fw_layout.h` (the updater,
which takes every flash address from this table and never from the host).
At run time the kernel's partitions must match it name for name and size for
size, or the camera refuses to update.

The current build is 0.9 MiB of kernel (73% of a slot) and 1.4 MiB of rootfs
(78%). `make` fails if either outgrows its slot.

## Build outputs

`output/k1cam/images/`:

- `k1cam.bin`: the whole 8 MiB flash, both slots holding this build, a fresh
  environment and an empty data partition. For installation only: flashing it
  resets the settings.
- `k1cam.upd`: the routine update, this kernel and rootfs in a checked
  bundle (format below), followed by the build's os-release for whoever
  picks the file.
- `k1cam.bin.sha256sum`, `k1cam.upd.sha256sum`, and `k1cam-layout.txt`.

`post-image.sh` reads every part of `k1cam.bin` back and compares it with its
source, checks that the rest of each partition is erased, parses
`k1cam.upd` back with the camera's own parser, and reads its os-release back.

## Using it

```sh
uvcdctl firmware info                 # build, layout, slots, how it booted
uvcdctl firmware dump k1-backup.bin   # the whole flash, verified
uvcdctl firmware upgrade output/k1cam/images/k1cam.upd
```

`uvcdctl help firmware` and `package/k1cam-uvcd/uvcdctl/README.md` describe
the commands, their safety checks and exit statuses. An upgrade stops with
nothing changed if the bundle is damaged, for another board or layout, too
big, or the same build; if a trial is pending; or if the camera is not
running its confirmed slot.

On the camera itself, `fw_printenv` shows the slot variables, and the login
banner shows the running slot and how it booted.

## Installing the A/B layout

The A/B layout moves the rootfs and data partitions, so it cannot be
installed by an update; the camera has to be flashed with `k1cam.bin`, which
is the user's job, with the usual tools. Before that:

1. Save what you want to keep from the data partition, which the new image
   replaces: at least `/etc/uvcd.conf` (the controls), and anything else you
   put on the camera (a custom sensor tuning under `/etc/sensor`, files in
   `/opt`). The new data partition is 1600 KiB instead of 3584 KiB.
2. Flash `k1cam.bin`. Both slots hold the same build, so whichever U-Boot
   picks boots.
3. Copy the saved files back from the camera's shell; they land in slot a's
   settings layer. `uvcdctl firmware info` should report
   `layout=k1cam-ab-8m-v1` and `boot=confirmed`.

From then on, updates use `k1cam.upd`. Keep a way to flash the camera (the
USB boot ROM and cloner) for anything that breaks the bootloader or both
slots; the A/B scheme does not protect U-Boot.

## How an update works

1. `uvcdctl` checks the bundle and asks the camera for its state.
2. The upload: the camera keeps the bundle in RAM (checked free memory
   first), 44 bytes per DATA frame, each acknowledged in STATUS.
3. `FINISH_UPGRADE`: on a worker thread, so the camera keeps answering USB,
   the camera checks the whole bundle again (its digest, the manifest, each
   component's SHA-256, the uImage header and data CRCs, the SquashFS
   superblock, board, layout and boot compatibility) and whether its boot
   state still allows an update. Nothing is erased before all of that passes.
4. It erases the inactive slot's kernel and rootfs partitions whole, writes
   them, and reads them back: the images must match their digests and the
   rest of each partition must be erased.
5. It gives the new slot a fresh settings layer with a copy of the running
   slot's `/etc/uvcd.conf` and `.good`, nothing else.
6. Only now, one environment write schedules the trial: `trial_slot`,
   `upgrade_available=1`, `bootcount=0`. Up to here a failure, an abort or a
   power cut leaves the boot state as it was.
7. The camera reboots (unless `--no-reboot`). U-Boot boots the trial
   (below); uvcd in the trial checks the camera and confirms the slot.

## Boot logic

U-Boot runs `bootcmd=run k1_boot` from `uenv.txt`; each step is a named
script, and the variables are the whole state:

| State | Variables | Boot |
| --- | --- | --- |
| normal | `active_slot=a`, no `trial_slot`, `upgrade_available=0` | slot a, `k1cam.boot=confirmed` |
| trial scheduled or running | `active_slot=a trial_slot=b upgrade_available=1 bootcount=0..2` | slot b, `k1cam.boot=trial` |
| trial exhausted | as above, `bootcount=3` | trial dropped, slot a |
| confirmed | `active_slot=b`, no trial, `bootcount=0` | slot b |

- Before a trial boot, `bootcount` is incremented and saved with `saveenv`;
  if that save fails the trial is not booted (U-Boot's own BOOTCOUNT_ENV
  ignores save errors, so the counting is done in the script instead).
- After `bootlimit` (3) trial boots the trial is dropped (`k1_reject` clears
  it and saves) and the confirmed slot boots.
- A trial kernel that won't load or verify is dropped at once, and the
  confirmed slot boots in the same power-on.
- A trial boots under the hardware watchdog, started by U-Boot with a
  nominal 120 s that is really 84 s: U-Boot assumes a 32 kHz clock, the
  WDT runs at 24 MHz/512 (measured). It keeps running in Linux, and
  nothing services it until the slot is confirmed.
- Invalid or contradictory variables (a trial of the confirmed slot, a slot
  that is not a or b, no `bootlimit`) never boot a trial; an invalid
  `active_slot` boots slot a.
- If the confirmed slot's kernel won't load, U-Boot tries the other slot
  (`k1cam.boot=fallback`, under the watchdog) and otherwise stops at its
  prompt for serial recovery.
- The environment is redundant: env_a and env_b alternate on every save,
  each write complete before the older copy is marked obsolete, so a power
  cut during a save leaves the old or the new environment, never neither.
  With both copies gone U-Boot uses its built-in copy of `uenv.txt` (slot a).
- The kernel command line names the slot: `root=/dev/mtdblock4|6`,
  `k1cam.slot=a|b`, `k1cam.boot=confirmed|trial|fallback`,
  `k1cam.layout=k1cam-ab-8m-v1`.

## The trial in Linux

- `/init` mounts the data partition and, as the writable layer over the
  slot's rootfs, `slots/<slot>/root` on it: each slot has its own settings,
  so neither release sees the other's files or whiteouts, and going back
  restores the old slot's settings as they were. `/opt` is shared. If
  `k1cam.slot` is missing or does not match the root device, or the data
  partition won't mount, the system runs on a RAM layer (settings don't
  persist) instead of mixing or erasing anything; the partition is only
  formatted when it is entirely blank.
- `K99watchdog` does not start the watchdog daemon in an unconfirmed trial.
- uvcd, once the camera is up, runs its self-test: a 640x360 H.264 stream
  until five valid frames including a keyframe, then MJPEG the same, with no
  host needed (a host's healthy stream of three seconds counts too). Then it
  checks that the data partition is writable and that `/` runs on this
  slot's layer, writes `active_slot=<slot>`, clears the trial and
  `bootcount` in one environment save, reads it back, and writes
  `/run/k1cam/confirmed`.
- `k1cam-trial-gate` starts the watchdog daemon on `confirmed`. On
  `/run/k1cam/trial-failed` (uvcd's self-test failed three tries, or the
  confirmation could not be saved), or no confirmation within 75 s of boot,
  it reboots so U-Boot counts the attempt. A hang anywhere is left to the
  watchdog.

## Dumps

A dump copies the 8 MiB `all` partition into RAM, if enough is free (8 MiB
plus a 6 MiB reserve), with uvcd's settings writes held and the jffs2 garbage
collector stopped (SIGSTOP), then reads the flash again to check that nothing
changed while it copied; the manifest says `consistency=snapshot`, or
`unverified` if the flash kept changing over three tries. Other writers
(someone editing files on the camera's shell) are not stopped. With too
little RAM the camera refuses, unless the host asks for a live dump, which
reads the flash as it goes and is labelled `consistency=live` with no
camera-side digest. Remounting the data partition read-only would have been
the stronger freeze, but this kernel's overlayfs holds write access to its
upper filesystem for as long as it is mounted, so that remount always fails.

## Protocol, version 1

`package/k1cam-uvcd/src/uvcd_firmware.h` is the specification. In short:

- COMMAND (26, SET), DATA (27, SET and GET), STATUS (28, GET), 60 bytes
  each, little-endian, reserved bytes zero. 60 is the most the kernel's UVC
  gadget passes to uvcd (`UVC_MAX_REQUEST_SIZE`, kernel commit 64dc16da1).
- Every COMMAND and DATA frame carries a host-chosen sequence number; its
  outcome is the STATUS with that sequence (`RESULT` 0 or an error). A
  repeated frame keeps its first result and is not executed twice; the same
  sequence with other bytes is rejected. One frame at a time.
- Commands: INFO, READ (select a 44-byte window: the dump, its manifest, or
  the information record), BEGIN_DUMP, END_DUMP, BEGIN_UPGRADE (length and
  SHA-256), FINISH_UPGRADE, REBOOT, ABORT (with UNSCHEDULE: withdraw a
  trial). The camera issues the session ids.
- DATA frames: session, offset, sequence, length (up to 44), kind, CRC-16.
  An upload must arrive in order; a resend of bytes already received is
  accepted if identical, and a gap or a conflicting resend ends the upload.
- STATUS: phase, progress, the last frame's result, the operation's error and
  errno detail, features, suggested poll interval, slots, boot state.

Sessions the host abandons are released after 60 s (upload) or 120 s (dump),
and USB disconnects end them. Once `FINISH_UPGRADE` is accepted the install
finishes even if the host goes away; it can be aborted until the new slot's
settings are prepared (the point of no return before scheduling).

## Update bundle, version 1

`package/k1cam-uvcd/src/fw_bundle.h` is the specification: a 256-byte header
(magic `K1CAMUPD`, version, board `k1cam-t31l-gc2083`, layout
`k1cam-ab-8m-v1`, build id, lowest boot compatibility, length, and a SHA-256
over header and records), two 64-byte component records (kernel, then
rootfs: offset, length, SHA-256), and the payloads. `k1cam-mkupd` builds it
(and `--check` parses it); uvcd and uvcdctl parse it with the same code;
`package/k1cam-uvcd/tests/bundle_check.py` is an independent reading of the
format from its description. The digests detect corruption, they do not
authenticate the publisher.

After the bundle, `k1cam.upd` carries the release information: magic
`K1CAMINF`, the text's length, the bundle's manifest digest (so it can't be
taken for another bundle's), a SHA-256 of the text, and the build's
`/usr/lib/os-release` (`k1cam-mkupd -i`; `--check` prints it as `info:`
lines). It is for the person choosing the file, as k1cam-flasher shows it:
hosts send the camera only the bundle, whose length field says where it
ends, so the camera never sees it and firmware from before it updates from
these files too. Host tools from before it refuse them (bytes after the
bundle).

## Tests

- `package/k1cam-uvcd/tests/run.sh` (after `make`): host tests under ASan
  and UBSan. `fw_test` covers SHA-256, the CRCs, the bundle format (golden
  vector shared with `bundle_check.py`, mutations, truncations, random
  corruption), the flash helpers, and the environment against U-Boot's
  mkenvimage, including a power cut at every step of a save. `update_test`
  drives the protocol as the gadget does against an 8 MiB NOR model (32 KiB
  erase blocks as on the camera, programming only clearing bits): frames
  written out by hand from the header's description, dumps, uploads, every
  rejection, flash failures, aborts, the trial confirmation, a power cut at
  every flash operation of an install (after which the confirmed slot must be
  intact and a scheduled trial complete), and 50000 random frames. `uvcdctl_test`
  runs `uvcdctl firmware` against that same camera-side code, reboot and
  rollback included. `gadget_test` covers the routing and the self-test.
- `board/k1cam/tests/boot-test.sh`: the boot logic in U-Boot's sandbox,
  built from the camera's U-Boot release with the camera's environment
  setup (redundant copies in an emulated 8 MiB SPI NOR, the old hush
  parser, `uenv.txt` as the default environment), against flash images that
  uvcd's environment code also reads and writes.
- `board/k1cam/tests/image-test.sh`: `post-image.sh` on the build's own
  images, its outputs checked independently (placement, erased gaps, the
  environment copies, checksums, `bundle_check.py`, a deterministic bundle),
  and the layout mistakes it must refuse.

## Established on hardware

On the dev camera, 2026-10-19, after flashing `k1cam.bin`:

- The kernel enumerates with selectors 26-28; `uvcdctl list` is unchanged;
  `firmware info` and `status` work.
- Dump: 8 MiB in 164-182 s (47-54 KiB/s), `consistency=snapshot`. Every
  partition except `data` matched the flashed `k1cam.bin` byte for byte.
  During a 1080p30 H.264 stream it is just as fast and the stream keeps
  30 fps; MemFree is then 15.3 MiB (22 MiB counting the page cache, which
  is what uvcd counts) against the 14 MiB needed.
- Update to a new build: 41 s upload (57 KiB/s), 61 s in all to a
  confirmed slot b. uvcd's self-test takes under 1 s and the slot is
  confirmed about 4 s after the kernel starts. Back to slot a likewise,
  with `/etc/uvcd.conf` carried to the new slot's layer.
- Rollback: a bundle whose `/init` hangs. U-Boot's watchdog is still
  enabled in Linux (TCER 1, counter running at 733 counts/s: RTC clock,
  divider 64, so the nominal 120 s is 84 s). Three watchdog resets, then
  slot b again, confirmed, with its settings; `uvcdctl` exited 3 after
  5 min 19 s.
- Environment: with the newest copy's CRC zeroed, U-Boot and `fw_printenv`
  used the other copy (here the state of the first trial boot), booted the
  trial again, and the save rewrote the damaged copy.
- Six soft and watchdog resets in a row all booted normally.

Still open:

1. The power-cut cases of the plan: during upload, erase, programming,
   verification, the trial scheduling, the trial boot and its confirmation.
2. Whether the watchdog's real 84 s should become 120 s (U-Boot's clock
   rate) or stay; the gate's 75 s reboot comes first either way.

A known risk: a soft reboot of the K1 has twice landed in the boot ROM's USB
mode instead of booting (2026-10-01, cause not found; a power cycle
recovers it). A trial reboot, a watchdog reset or a rollback are all such
resets. If it recurs, the update logic is still safe (the state is in the
environment and resumes on the next power-up), but the camera will need a
power cycle to get there.
