# USB identity

k1cam uses `f055:4b31` for its UVC + CDC ACM composite device, including
camera controls, flash dumps and A/B updates over the UVC extension unit.

| Field | Value |
| --- | --- |
| Vendor ID | `0xf055` (unofficial FOSS community convention) |
| Product ID | `0x4b31` (ASCII "K1") |
| Manufacturer | `k1cam` |
| Product | `K1 Camera` |
| Serial | Existing per-camera serial identity |

This pair is a project convention, not an exclusive allocation or USB-IF
certification. Global uniqueness cannot be guaranteed. The
[community registry](https://f055.io/) describes the informal `0xf055`
namespace; it does not establish a USB-IF assignment.

## Collision checks

On October 19, 2026, the community registry had no entry for `4b31`, exact
web searches found no identified device using `f055:4b31`, and the local
thingino-linux USB and USB-media driver tables contained no matching
VID/PID entry. These checks reduce the chance of an existing driver quirk
or product collision; they do not reserve the pair or prove nobody uses it.
No allocation request has been submitted.

Hosts should use the manufacturer/product strings and the camera's UVC
extension-unit protocol as additional identification. `uvcdctl` already
probes that protocol rather than filtering on VID/PID. The flasher's Linux
transport also probes the protocol; its libusb transport verifies the
firmware control's frame size before sending commands. The new flasher
udev rule matches the strings as well as the numeric IDs.

## Migration

Earlier firmware used `1d6b:0110`. The flasher recognizes both pairs so
updates and rollback can cross the identity change. Install the updated
flasher and udev rules before updating the camera. Numeric-ID-based host
rules and device paths may need updating; the existing manufacturer,
product and serial strings are retained.

The descriptor is in `drivers/usb/gadget/webcam.c` in
[itzexor/thingino-linux](https://github.com/itzexor/thingino-linux/tree/k1cam).
The firmware fetches the kernel commit pinned in `configs/k1cam_defconfig`.
An A/B update includes that kernel and can deliver the new identity;
changing a sibling kernel checkout alone does not change a firmware image.

The Ingenic boot ROM retains `a108:c309` in cloner mode. Initial installation
and recovery use that identity independently of the running firmware.

If an exclusive allocation is needed later, request one from a VID owner
such as [Openmoko](https://github.com/openmoko/openmoko-usb-oui) or
[pid.codes](https://pid.codes/howto/), with the firmware's precompiled
Ingenic dependencies disclosed, or obtain a dedicated VID from
[USB-IF](https://www.usb.org/getting-vendor-id).
