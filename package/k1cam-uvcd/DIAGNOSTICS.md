# ISP diagnostic channel v1

This channel is independent of the firmware exchange (XU selectors 26-28).
It samples ISP getters and uvcd's accepted control state in the main loop,
without writes to the ISP, persistent settings, frame waits, or capture startup.
It is a coherent copy of daemon state; the individual ISP getters and the
video are sampled separately, not synchronized to one sensor frame.

## USB transport

Extension Unit 4 adds selectors 29 and 30. The kernel descriptor in
`drivers/usb/gadget/webcam.c` must advertise 23 controls and bitmap
`7f b3 3f 3e`; retired selectors remain clear. A daemon update alone cannot
expose these controls through uvcvideo. Re-enumerate after updating the kernel.

| Selector | GET_INFO | GET_LEN | GET_CUR | SET_CUR |
|---|---|---|---|---|
| 29, DIAG_LATCH | GET + SET (3) | 1 | layout version (1) | exactly 1 ignored byte: capture, rewind to page 0 |
| 30, DIAG_PAGE | GET (1) | 60 | next page, below | unsupported |

GET_LEN uses a little-endian u16 response. GET_CUR requires the declared
length exactly; a short page stalls without advancing. Other requests stall.
Before any latch, a page is 60 zero bytes (count 0).

Each 60-byte page contains:

| Byte | Meaning |
|---|---|
| 0 | zero-based page index |
| 1 | total page count (14 for v1) |
| 2-3 | snapshot sequence, little-endian u16 |
| 4-59 | next 56 record bytes; final page zero-padded |

After the final page, the cursor wraps to 0 without sampling again. Every
capture increments a wrapping u16 counter; the page ID and record ID agree.
Console captures increment the counter but leave the USB record and cursor
alone. The single USB latch is shared by readers: sequence/index/count
validation detects interleaving, rather than returning mixed statistics.

A reader checks GET_LEN on 30 and GET_CUR on 29, latches, then validates
index, count and sequence on every page and the sequence and size in the
assembled record. Current clients allow at most 32 pages and retry an
inconsistent sequence once. A transport failure remains an error; an absent
selector or unsupported version means the client can use estimates.

## Record

All integers are unsigned, little-endian. Size is 745 bytes. The wire format
uses explicit byte serialization, never native struct layout. Append-only
fields retain v1; changing existing fields requires a version bump. Readers
use record size/page count and ignore appended fields they do not understand.

| Offset | Bytes | Field |
|---|---|---|
| 0 | 1 | layout version, 1 |
| 1 | 1 | flags: bit 0 AE stable, 1 auto exposure enabled, 2 auto WB enabled, 3 valid guard sample |
| 2 | 2 | snapshot sequence |
| 4 | 2 | total record size, 745 |
| 6 | 2 | measurement validity mask, below |
| 8 | 8 | capture start time, CLOCK_MONOTONIC milliseconds on camera |
| 16 | 1 | accepted metering mode 0-4; 255 when not valid |
| 17 | 1 | requested exposure compensation |
| 18 | 1 | last compensation accepted by the ISP |
| 19 | 1 | last guard top-1% bin, whole sensor |
| 20 | 2 | AE target (saturated at 65535) |
| 22 | 2 | AE mean (saturated at 65535) |
| 24 | 4 | exposure time, microseconds |
| 28 | 4 | total gain, x256 |
| 32 | 2 | AWB colour temperature, Kelvin (not clamped to manual control range) |
| 34 | 2 | sensor width |
| 36 | 2 | sensor height |
| 38 | 2 | view x |
| 40 | 2 | view y |
| 42 | 2 | view width |
| 44 | 2 | view height |
| 46 | 2 | effective zoom, x100 |
| 48 | 2 | rotation, 0 or 180 degrees |
| 50 | 2 | last guard level in view, x100 (top bin multiplied by view share) |
| 52 | 2 | accepted guard compensation cut, x10000 |
| 54 | 113 | 225 zone weights, 0-8, low nibble first; final high nibble zero |
| 167 | 450 | 225 zone levels, u16, saturated at 65535 |
| 617 | 128 | 64 histogram bins, u16 |

Zones use ISP raster order `row * 15 + column`; the view uses the same sensor
coordinate system. Rotation reports the control state, not an extra transform
to apply blindly to the tables. Alignment under rotation should be checked
on hardware. The histogram covers the **whole sensor**, including pixels
outside the view. Original 256 bins are summed in groups of four using u64,
then normalized so the largest is 65535. These are relative counts, not exact
pixel counts. Linear ISP levels are distinct from gamma-processed JPEG luma.

Validity mask (a set bit means the associated fields are available):

| Bit | Fields |
|---|---|
| 0 | AE stable, target and mean |
| 1 | exposure time and gain (both nonzero) |
| 2 | AWB estimate |
| 3 | sensor/view geometry and effective zoom |
| 4 | accepted zone weights |
| 5 | zone levels |
| 6 | nonempty linear histogram |
| 7 | last accepted compensation |
| 8 | guard top bin, level and cut |

Failed or absent getters clear only their validity group and leave its bytes
zero. Camera down yields validity 0 and metering 255. Mode flags and requested
controls remain readable. Guard validity requires Subject metering, automatic
exposure, and a successful prior guard sample for the current metering setup.
The reported cut is the accepted one even when a newer write was rejected.

## Console and remote access

`uvcdctl diag` on the camera captures through `/var/run/uvcd.sock`. On a Linux
USB host, `uvcdctl --device /dev/videoN diag` latches and reads the same record.
Both print `name=value` lines, 15 rows each of `weights_00`...`weights_14` and
`levels_00`...`levels_14`, plus `histogram` with 64 values. `valid` controls how
the values should be interpreted. The local socket response ends with `ok`.

The flasher daemon adds authenticated, session-scoped
`GET /api/v1/tuning/diagnostic`:

- 200, application/octet-stream: one complete wire record, without USB page headers;
- 204: firmware does not support diagnostics;
- usual tuning errors for busy device or failed reads.

The request shares the existing tuning mutex and camera/job exclusion. It
never starts video. An older daemon's 404 also means unavailable. This is an
additive API v1 endpoint; older clients continue to work.

Remote JPEG requests can add `diagnostic=1`. The daemon samples before
closing a manual capture, while its view is still active, and includes the
record as hex in the `K1cam-Diagnostic` response header. An absent header means
unavailable; `error` means a diagnostic read failed without losing the JPEG.
This prevents manual snapshots from being overlaid with the camera's idle
crop. Existing clients and daemons can ignore the new header/query parameter.

For local live video the GUI polls at 2 Hz only while Tuning is visible and
Metering, Clipping or Histogram is enabled. Remote diagnostics are requested
with each image only when an overlay needs them: at 2 Hz in Auto snapshot,
or on a manual snapshot request. It allows one outstanding request, backs off
after errors, and does not cache a missing feature across reconnects. Local
ISP samples older than 1.5 seconds, or shown over video that old, are not used.
A remote image retains its paired diagnostic record for as long as that
image is shown; the legend identifies it as ISP data at the snapshot.
Metering uses actual weights, levels and crop; the histogram uses linear ISP
data and
marks guard threshold 200 and the last guard level in view. The legend labels
partial/unavailable data and picture estimates. Clipping stripes always come
from JPEG pixels. Video and diagnostics are not a frame-matched pair.

## Validation

`tests/run.sh` covers HAL success/missing getters, camera down, nibble/zone
order, saturation, histogram overflow, paging/padding/wrap, malformed lengths,
console reads between USB pages and firmware-status isolation. `uvcdctl` tests
cover USB output and mixed sequence rejection. Flasher tests cover parsing,
malformed/mixed pages, append-only records, authenticated remote round trips,
and automatic-control refresh while diagnostics are being polled.

After publishing the kernel descriptor change and bumping the firmware pin,
follow the repository's `make clean all` workflow. On hardware verify USB
re-enumeration, `uvcdctl diag`, subject easing, changing illumination, crop and
rotation alignment, and local/remote overlays. No camera is flashed by tests.
