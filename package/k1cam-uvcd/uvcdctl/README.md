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
uvcdctl list
uvcdctl describe [control]
uvcdctl get <control>
uvcdctl set <control> <value>
uvcdctl keyframe
uvcdctl reset
```

`list` displays every control's ID, current value, range, and default. On a
terminal, values that differ from the default are highlighted (set `NO_COLOR`
to turn this off).
`describe` explains what each control's values mean; it reads nothing from the
camera, so it works without one attached:

```
 ID  NAME                         VALUES
 40  h264-bitrate-kbps            0: automatic, 1-16000: target kbps
```

Control names and numeric IDs are accepted by `get`, `set`, and `describe`.
`get` and `set` print `name=value`.

## Persistence

Every change persists, whichever program makes it: `uvcdctl`, `v4l2-ctl`, or
an application's own camera settings. The camera writes accepted changes to
`/etc/uvcd.conf` about two seconds after the last one and loads the file at
startup. There is no save step.

`reset` restores every control to its factory default and persists that
immediately.

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
uvcdctl describe h264-gop-frames
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

By default, `uvcdctl` selects the `uvcvideo` node that answers an Extension
Unit 4 query. Use `--device /dev/videoN` to select a device explicitly.

Exit status is 0 on success, 1 on a device error, 2 on a usage error.
