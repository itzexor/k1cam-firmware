#!/bin/sh
# post-build.sh -- trim this board's rootfs down to a USB webcam.
#
# Runs after thingino's scripts/rootfs_script.sh (see
# BR2_ROOTFS_POST_BUILD_SCRIPT in the defconfig), with Buildroot's target
# directory as $1.
#
# The Creality K1 camera is USB-powered, has no network and no LEDs, and
# answers to exactly one thing: a USB host opening it as a UVC webcam. What
# matters is how fast the host sees it -- a native UVC camera enumerates in
# about a second. So this removes every boot service that has no job on such
# a device, rather than leaving them to run and exit.
#
# Most of these come from thingino's global overlay/, which every build gets
# unconditionally and which no Kconfig option controls -- setting a package
# to =n in the defconfig does not stop its init script from being installed.
# That is why this is done here, by file, instead of with config options.
#
# Everything here is idempotent: the overlay is re-copied on every build, and
# this script simply removes the same files again.

set -e

TARGET_DIR=${1:?usage: post-build.sh TARGET_DIR}
INITD="$TARGET_DIR/etc/init.d"

# Services with nothing to do on this board. Grouped by why.
#
#   network I/O:  no interface ever comes up, so none of these can work
#   time:         no network time source; nothing on the device needs a
#                 correct wall clock (F01datetime only sets a build-date floor)
#   sysadmin:     identity, logging, scheduling and update machinery for a
#                 managed IP camera. uvcd logs to the kernel ring buffer
#                 instead of syslog, so `dmesg` replaces logread.
#   leds:         the board has none
#   duplicate:    S02sysctl is busybox's own copy of S00sysctl
for script in \
	S30dropbear S50dropbear S40network S41ifplugd S43mounts S05dns \
	S48webui-config S50mdnsd S91mqttsub \
	F01datetime S01timezone S49ntpd \
	S01syslogd S01seedrng S03mac S04hostname S50crond S94rc.local \
	S97sysupgrade \
	S00blink S99led \
	S02sysctl; do
	rm -f "$INITD/$script"
done

# No microphone on this board. S11modules loads everything listed in
# modules.d, and loading a driver for absent hardware is boot time spent for
# nothing. The ISP, AVPU and sensor entries stay.
rm -f "$TARGET_DIR/etc/modules.d/40-audio"

# thingino's F02failsafe waits two seconds on every boot for an [f] on the
# UART before letting the system start. This board's emergency console is the
# USB ACM getty below, so that wait is pure delay before enumeration.
#
# It cannot simply be deleted: F02failsafe is also what runs rcS. inittab only
# runs rcF, so without it no S* script would ever start.
cat >"$INITD/F02failsafe" <<'EOF'
#!/bin/sh
# Replaces thingino's F02failsafe on this board (see configs/cameras/
# creality_k1_t31l_gc2083/post-build.sh): no two-second [f] wait on the UART,
# just the part that matters -- starting rcS.
case "$1" in
start)
	exec /etc/init.d/rcS
	;;
esac
exit 0
EOF
chmod 0755 "$INITD/F02failsafe"

# Emergency console on the USB ACM port (host: /dev/ttyACM0).
#
# Supervised by init, next to the UART getty, rather than by uvcd's init
# script: the console is board policy, not part of the webcam, and init
# brings it back by itself whenever the host re-enumerates the device.
#
# init only starts respawn entries once sysinit -- the whole boot, rcF and
# rcS included -- has finished, so none of this is on the path to
# enumeration. busybox init does not rate-limit respawns, so the entry
# throttles itself: /dev/ttyGS0 only exists once uvcd's init script has loaded
# g_webcam, and a getty that fails must not spin. -L because a USB serial line
# has no carrier to wait for.
INITTAB="$TARGET_DIR/etc/inittab"
if ! grep -q 'ttyGS0' "$INITTAB"; then
	cat >>"$INITTAB" <<'EOF'
# USB ACM emergency console (added by creality_k1_t31l_gc2083/post-build.sh)
::respawn:/bin/sh -c 'while [ ! -e /dev/ttyGS0 ]; do sleep 1; done; /sbin/getty -L -n -l /bin/sh ttyGS0 0 vt100; sleep 1'
EOF
fi
