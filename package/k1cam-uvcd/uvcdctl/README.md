# uvcdctl

`uvcdctl` is a Linux command-line utility for configuring `k1cam-uvcd`
cameras through V4L2 and UVC controls. It uses one V4L2 device file and
supports standard V4L2 controls and UVC Extension Unit controls.

The standard controls come from two UVC units: the Processing Unit (picture
adjustments, white balance) and the Camera Terminal (exposure, exposure
priority, digital zoom and pan/tilt). `list` shows them as `pu` and `ct`, and
Extension Unit controls as `xu`. Image rotation is an Extension Unit control:
UVC defines one, but V4L2 has no control for it, so Linux could not reach it
on the standard path.

## Commands

```sh
uvcdctl list [control]
uvcdctl describe [control]
uvcdctl get <control>
uvcdctl set <control> <value>
uvcdctl keyframe
uvcdctl reset
uvcdctl firmware info
uvcdctl firmware status
uvcdctl firmware dump [--live] FILE
uvcdctl firmware upgrade [--no-reboot] [--wait-timeout SECONDS] FILE
uvcdctl firmware reboot
uvcdctl firmware cancel
uvcdctl help [command]
```

`help` shows the command list; `help <command>` shows that command's syntax,
description, and examples without accessing a camera. For example, use
`uvcdctl help set` for writing controls. `get` and `set` without their required
arguments print their own usage to standard error and exit with status 2.
`<command> --help` and `<command> -h` also show command-specific help.

`list` and `describe` share the same listing: each control's ID, unit, current
value, range, and default. `describe` also includes a description column
explaining the values. Pass a control name or numeric ID to either command to
show just that control; omit it to show all controls. Both commands read from
the camera. On a terminal, values that differ from the default are highlighted
(set `NO_COLOR` to turn this off). For example, `describe h264-bitrate-kbps`:

```
 ID  NAME                         UNIT  VALUE    MIN    MAX DEFAULT  DESCRIPTION
 40  h264-bitrate-kbps             xu        0      0  16000       0  0: automatic, 1-16000: target kbps
```

Control names and numeric IDs are accepted by `get`, `set`, `list`, and `describe`.
`get` and `set` print `name=value`. After a USB write, `set` checks the
returned value and exits with status 1 if it differs or cannot be read.
Standard V4L2 controls can be cached by the host driver; this is not an
independent ISP readback. Settings for an inactive mode or format are staged
for when it is used.

## Firmware

The `firmware` commands read and update the camera's flash over the same
device node, through three 60-byte controls on Extension Unit 4 (selectors
26-28; the protocol is described in `../src/uvcd_firmware.h` and
`FIRMWARE-UPDATE.md` at the top of the tree). A camera whose firmware
predates them says so and exits with status 1.

`firmware info` prints what the camera reports as `key=value` lines: board,
flash layout, build id, kernel, serial number, how it booted (`confirmed`,
`trial`, `fallback`, or `single` on the original layout), its running,
confirmed and trial slots, and the features it offers now. `firmware status`
reads the protocol's current state without changing anything: the phase and
progress of the current operation and the result of the last command.

`firmware dump FILE` saves the whole 8 MiB flash. The camera first copies it
to RAM with its own settings writes and the flash filesystem's background
writer paused, reading it twice to make sure nothing changed meanwhile, and
the dump is checked against the camera's SHA-256 of that copy. Data goes to
`FILE.part`, which becomes `FILE` only once complete and verified; a failed
dump leaves the partial file and says so, and neither file is ever
overwritten. If the camera lacks the RAM for a copy, the dump is refused;
`--live` then reads the flash in place, a diagnostic dump that may be
inconsistent and is labelled so. The transfer is slow -- every 44 bytes is
two USB control requests -- so expect minutes; the rate is printed at the end.

`firmware upgrade FILE` installs a `k1cam.upd` bundle (built next to
`k1cam.bin`). It needs a camera on the A/B flash layout. It checks the bundle
completely before contacting the camera, and the camera checks it again: the
bundle's digests, the kernel's uImage CRCs, the SquashFS header, the board,
the layout, and that it fits. The camera writes it into the slot that is
neither running nor confirmed, reads it back, prepares that slot's settings
(a copy of the current `/etc/uvcd.conf`), and schedules it as a trial. Then
it reboots into the trial; the trial confirms itself once it has encoded
video, and `uvcdctl` finds the camera again by its serial number and reports
the outcome:

- 0: confirmed, the camera runs the new build (with `--no-reboot`: staged;
  `firmware reboot` or any reboot starts the trial);
- 1: failed before a trial was scheduled: the camera still boots what it ran;
- 3: rolled back: the trial failed and the camera runs the old build again;
- 4: not confirmed: still pending when `--wait-timeout` (600 s) ran out, or
  the camera did not come back.

A trial that fails three boots, or whose kernel won't load, goes back to the
confirmed slot. `firmware cancel` stops a dump or an upload, an install
before it schedules the trial, or withdraws a scheduled trial. An update
never writes the running or confirmed slot, the bootloader, its environment
other than the slot variables, or the settings partition. The bundle's
digests detect damage; they do not prove who made it.

Gain ceilings use the T31 SDK's logarithmic units: 0 is 1x, and every
32 units doubles gain. `max-analog-gain` accepts 0-224, with the unchanged
default of 160 (32x). A ceiling of 224 (128x) allows the GC2083 to reach
its last analog step, about 125.4x. `max-digital-gain` still accepts 0-160,
with a default of 80 (about 5.66x).

## Persistence

Every change persists, whichever program makes it: `uvcdctl`, `v4l2-ctl`, or
an application's own camera settings. The camera writes accepted changes to
`/etc/uvcd.conf` about two seconds after the last one and loads the file at
startup. There is no save step.

`reset` restores every control to its factory default and persists that
immediately. On the camera's local socket, a partial reset or a persistence
failure returns an error and leaves the last proven settings intact.
Crossed explicit H.264 QP bounds are rejected; an old config containing them
loads automatic bounds with a warning.

If a stream dies within ten seconds of starting, or of a change made while it
runs, the camera assumes the newest settings caused it. It restarts with the
settings the last healthy stream used, and after a second such failure in a
row, with factory defaults.

## Build

The standalone build runs on a Linux USB host and produces a native host
executable. It needs `make`, a C11 compiler, libc development files and Linux
userspace headers for V4L2/UVC. No libusb, camera SDK, Buildroot build or
submodule initialization is required. Keep the firmware checkout intact:
the Makefile also compiles shared bundle code from `../src`.

From a fresh checkout:

```sh
git clone --branch k1cam https://github.com/itzexor/k1cam-firmware.git
cd k1cam-firmware/package/k1cam-uvcd/uvcdctl
make
./uvcdctl help
```

Run the resulting binary as `./uvcdctl` from this directory, or optionally
install it to `/usr/bin/uvcdctl` with `sudo make install`. The firmware build
separately cross-compiles a copy for the camera.

See the [firmware README](../../../README.md#projects-in-the-ecosystem) for
how the firmware, kernel and
[k1cam-flasher](https://github.com/itzexor/k1cam-flasher) fit together.

## Use

Connect a camera running k1cam firmware over USB. The host user needs read
and write access to its `/dev/videoN` node, commonly through the `video`
group or desktop device permissions. The examples below use the installed
command; use `./uvcdctl` from the build directory if it is not installed.

```sh
uvcdctl list                  # values, ranges and defaults
uvcdctl describe h264-rate-control
uvcdctl get spatial-denoise
uvcdctl set 23 200            # same control, by ID
uvcdctl set h264-bitrate-kbps 6000
uvcdctl set rotation 180      # camera mounted upside down
uvcdctl set zoom 200          # 2x digital zoom, then move it:
uvcdctl set pan 18000
uvcdctl keyframe
uvcdctl reset
uvcdctl firmware info
uvcdctl firmware dump k1-backup.bin
uvcdctl firmware upgrade k1cam.upd
```

Zoom upscales part of the sensor image to the stream size, and the ISP can
only enlarge so far: the camera caps zoom at 290 for 1920x1080, and every
other size takes the full 400. (On firmware that leaves the ISP at its
100 MHz default the caps are 125 for 1920x1080, 240 for 1280x960 and 330
for 1280x720.) `get zoom` still reports what was set, so switching to a
smaller size gets the rest of it back.

On the camera, `uvcdctl` uses the daemon's local socket at `/var/run/uvcd.sock`.
On a USB host, it selects the `uvcvideo` node that answers an Extension Unit 4
query. Both paths use the same `list`/`describe` formatting. Local readings
come from the daemon's control state. Use `--device /dev/videoN` before the
command to select a V4L2 device explicitly, for example:

```sh
uvcdctl --device /dev/video0 list
```

Exit status is 0 on success, 1 on a device error, 2 on a usage error (and
for `firmware upgrade`, 3 and 4 as above).
