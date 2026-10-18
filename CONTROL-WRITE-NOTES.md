# Control write audit, 2026-10-18

Scope: fix the reproduced view-recovery and metering failures, expose analog
headroom, and record control-write observations for a later validation pass.
Base source: b975cb9f2. These notes distinguish host fault injection, hardware
readback, and image-effect testing; a successful setter alone is not evidence
that an image-processing module changed the picture.

## Changes in this pass

- Rejected zoom/pan/tilt changes reset the gadget reader and watchdog state
  after the pipeline restores the old stream. The first restored frame can
  be delivered instead of waiting for the previous sequence count.
- Leaving subject metering writes weights before restoring compensation.
  A rejected compensation write retains the guard cut and returns failure.
  Accepted weights are tracked, and invalid metering makes the periodic tick
  restore the stored mode after a partial failure. A rejected weight write
  leaves compensation untouched. This includes the tuning-weight mode.
- Analog gain range: 0-224; default remains 160. Digital range/default remain
  0-160/80. SDK units are 32 per doubling: 160 = 32x, 192 = 64x, 224 = 128x.
  The sensor driver's last analog step is 125.4375x (log2 Q16 value 456839),
  so 224 allows that step. CLI help and documentation describe these units.

## Hardware setup and method

The camera was reached through its physical USB0 serial console. Its original
image identifies as k1cam-subject-metering+34014fb, with kernel 0c1d650135d4.
It runs an older daemon that stops the ISP while idle. A rebuilt daemon from
this checkout, with temporary logging around camera ISP setters and matching
getters, was uploaded to /tmp and bind-mounted over /usr/bin/uvcd for testing.
The diagnostic logging is not part of the production source changes.

Original uvcd.conf and uvcd.conf.good were copied into camera RAM before the
service restart. The baseline was factory controls plus metering=4. Tests used
both host UVC requests and the camera's local control socket. No firmware was
flashed. The original daemon and configuration were restored afterward and
verified by SHA256 and the control list.

## Observed results

All sampled ISP setters returned zero, including the startup writes. Available
getters read back the requested values. No exposed ISP knob was found to
outright reject these representative in-range writes.

| Controls | Values/checks | Result and limits |
|---|---|---|
| Brightness, contrast, saturation, sharpness | 144, then original values | ISP scalar readbacks matched. |
| Hue | 16, -16, 0 | ISP readback 144, 112, 128; signed-to-offset translation works. |
| Anti-flicker | 50 Hz, 60 Hz, off | ISP enum readbacks 1, 2, 0. Banding was not measured. |
| Gamma | 120, 100 | Curve sum changed 346351 -> 366745 -> 346351; original curve restored. |
| White balance | Auto -> manual; 4000 K, 6000 K; auto | Mode and R/B gains changed. At 4000 K, R/B=374/552; at 6000 K, 501/389. Colour accuracy remains unmeasured. |
| Manual exposure | 100 and 333 in 100-us units | Subsequent telemetry showed 9976 and 33292 us. Sensor quantization and frame-delayed readback apply. |
| Exposure priority | 1, 0 | Accepted. No controlled light ladder or automatic rate-drop measurement in this pass. |
| Exposure compensation | 64, 176, 128, center metering | ISP readbacks matched. Subject-to-center failure behavior is covered by host fault injection. |
| Analog gain ceiling | 0, 160, 192, 224 | ISP readbacks matched. 224 persisted across a service restart and was reapplied/read back at startup. Actual 125x sensor gain and image quality were not measured. |
| Digital gain ceiling | 0, 160, 80 | ISP readbacks matched. This caps ISP digital gain; the sensor digital gain shown in procfs is a separate field. |
| Spatial / temporal denoise | Spatial 64/0; temporal 96/64 | Setters succeeded. Both pinned HAL getters return RSS_ERR_NOTSUP (-122); hardware values and image effects are not independently established by this audit. |
| Defective pixel correction / DRC | 64, 128 | ISP strength readbacks matched. Module enablement and visual effectiveness still need measurement. |
| Highlight suppression | 1, 5, 0 | ISP strength readbacks matched. Clipping reduction was not measured. |
| Rotation | 180, 0 | Both ISP flip bits changed to 1/1, then 0/0. |
| Metering | Tuning, average, center, spot, subject | Weight getters succeeded; full-view sums were 225/1800/880/72 for the first four modes; subject weights adapted. No forced clipping transition was used. |
| Zoom, pan, tilt | Zoom 200/100, pan/tilt +/-18000/0 | Accepted idle and during H.264 streaming; crop changes recreated channels and frames continued. Exact image geometry was not measured. |
| H.264 bitrate, RC, QP, profile | 1500 kbps/auto; RC 0/1/2; min QP 25/auto, max QP 40/auto; profiles 0/1/2 | Commands succeeded and encoder restarts completed. 640x480 at 15 fps captured 499 frames over ~35 s while changing settings/crops. Encoder attribute readbacks and bitrate/QP effectiveness were not instrumented. |
| MJPEG quality | 60, 100, 80 | Restarts completed; 640x480 at 15 fps captured 177 frames over ~12 s. Encoded quality was not measured. |
| Keyframe action | One host request | Accepted. Exact requested-IDR timing was not measured. |

Expected host-side rejections: white-balance temperature is inactive while
AWB is enabled (V4L2 EACCES); the CLI rejects analog gain 255 against the
advertised maximum 224. These are not failed ISP writes. Factory reset was
not exercised, to preserve the baseline.

## Deferred validation and fixes

1. Startup ignores most ISP setter return values. Add named diagnostics and
   decide which failures prevent startup versus disable a control; use real
   getters where available. Do not treat the stored control value as ISP
   readback. Immediate exposure getters can still describe the previous frame.
2. Denoise readback needs a supported SDK path or image measurements. The HAL
   getter stubs are missing functionality, not evidence that denoise setters
   are broken. DPC/DRC strength readback also does not prove a module is enabled.
3. Rotation has two writes without rollback. Exposure can change frame rate
   before a later exposure write fails; several callers ignore rate-setting
   failure. Audit these compound changes like the metering failure fixed here.
4. UVC data handlers log a rejected apply after accepting the SETUP/data
   transaction; local socket errors are clearer. Standard host controls may
   also read cached values. Define consistent error and effective-value
   reporting before relying on a CLI success/readback as validation.
5. Encoder changes are stored even when a live bitrate apply or restart fails.
   Crossed QP bounds fall back to SDK defaults, while getters retain the
   requested bounds. Check encoder attributes and actual output, including
   failure recovery, inactive-format changes and effective crop limits.
6. Tuning metering without a captured reference still reports success without
   restoring weights (source finding, not reproduced on this camera). Gamma
   without a reference rejects live writes, but startup drops that error.
7. Config loading clamps ranges but skips the live normalization path and
   accepts numeric prefixes with trailing junk. Rotation and AE-mode values
   can therefore disagree with the hardware interpretation. Add shared
   normalization and parsing policy in the later validation pass.
8. CLI descriptions still say spatial denoise has default 192; actual default
   is 0. Keep descriptions and defaults consistent when doing knob cleanup.

## Verification and evidence

Host ASan/UBSan suites pass with new regressions for all three view controls,
weight/compensation failures in every non-subject metering mode, tick recovery,
and the analog/digital gain limits. Leak detection is disabled because this
execution environment prevents LeakSanitizer from running under tracing.
The rejected-view regression failed against the original gadget code before
its fix. The earlier targeted repro also demonstrated stale compensation.

CCACHE_DISABLE=1 make k1cam-uvcd-rebuild succeeded. Native uvcdctl compiled
with -Wall -Wextra -Wpedantic. The target link reports existing libpthread.so.0
and libdl.so.0 compatibility-name warnings; the uploaded binary ran with the
camera's installed compatibility libraries. Buildroot also reported missing
.files-list*.before inventory files during this incremental package rebuild.
No full flash image was built in this pass.

Session logs on the development host (temporary, not checked in):

- /tmp/k1cam-fixes-red.log, /tmp/k1cam-fixes-host.log,
  /tmp/k1cam-fixes-final-host.log, /tmp/k1cam-fixes-build.log
- /tmp/k1cam-audit-startup.log, /tmp/k1cam-audit-controls.log,
  /tmp/k1cam-audit-idle-readbacks.log
- /tmp/k1cam-audit-stream-controls.log,
  /tmp/k1cam-audit-stream-readbacks.log, /tmp/k1cam-audit-h264.log
- /tmp/k1cam-audit-mjpeg-controls.log, /tmp/k1cam-audit-mjpeg.log
- /tmp/k1cam-audit-gain-restart.log, /tmp/k1cam-audit-restored.log
