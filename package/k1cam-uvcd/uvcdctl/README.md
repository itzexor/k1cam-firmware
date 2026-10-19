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
```

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

```sh
make
sudo make install
```

## Use

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
come from the daemon's control state. Use `--device /dev/videoN` to select a
V4L2 device explicitly.

Exit status is 0 on success, 1 on a device error, 2 on a usage error.
