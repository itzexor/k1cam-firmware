#!/bin/sh
# post-build.sh -- trim this board's rootfs down to a USB webcam.
#
# Runs before and after thingino's scripts/rootfs_script.sh (see
# BR2_ROOTFS_POST_BUILD_SCRIPT in the defconfig), with Buildroot's target
# directory as $1. The first pass removes BusyBox's generated S02klogd before
# the global script validates init scripts; the final pass removes generic
# files that script recreates. Everything below is intentionally idempotent.
#
# The Creality K1 camera is USB-powered, has no network and no LEDs, and
# answers to exactly one thing: a USB host opening it as a UVC webcam. What
# matters is how fast the host sees it -- a native UVC camera enumerates in
# about a second. So this removes every boot service that has no job on such
# a device, rather than leaving them to run and exit.
#
# Most of these come from thingino's global overlay/, which Buildroot copies
# before any post-build script and every build gets
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
	S02klogd S30dropbear S50dropbear S40network S41ifplugd S43mounts S05dns \
	S48webui-config S50mdnsd S91mqttsub \
	F01datetime S01timezone S49ntpd \
	S01syslogd S01seedrng S03mac S04hostname S50crond S94rc.local \
	S97sysupgrade \
	S00blink S99led \
	S02sysctl S05usb; do
	rm -f "$INITD/$script"
done

# Files and libraries for features this hardware cannot use. Keep this list
# board-local: the global rootfs overlay still serves full network cameras.
rm -rf \
	"$TARGET_DIR/etc/cron" \
	"$TARGET_DIR/etc/dropbear" \
	"$TARGET_DIR/etc/network" \
	"$TARGET_DIR/etc/profile.d" \
	"$TARGET_DIR/etc/ssl" \
	"$TARGET_DIR/usr/share/udhcpc"
rm -f \
	"$TARGET_DIR/etc/default/ntp.conf" \
	"$TARGET_DIR/etc/default/resolv.conf" \
	"$TARGET_DIR/etc/banner" \
	"$TARGET_DIR/etc/cfg-backup.list" \
	"$TARGET_DIR/etc/ntpd_callback" \
	"$TARGET_DIR/etc/protocols" \
	"$TARGET_DIR/etc/rc.local" \
	"$TARGET_DIR/etc/rc.local.stop" \
	"$TARGET_DIR/etc/services" \
	"$TARGET_DIR/etc/thingino.json" \
	"$TARGET_DIR/etc/webrtc_profile.ini" \
	"$TARGET_DIR/usr/bin/jct" \
	"$TARGET_DIR/usr/lib/libjct.so"* \
	"$TARGET_DIR/usr/lib/modules"/*/ingenic/gpio-userkeys.ko

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

# RAM-only logs. klogd feeds kernel/uvcd messages into BusyBox syslogd's
# 64 KiB shared-memory ring, viewed with `logread` or `logread -f`. Nothing
# is written to flash and no network logger is involved.
cat >"$INITD/S01syslogd" <<'EOF'
#!/bin/sh
case "$1" in
start)
	start-stop-daemon -S -b -m -p /run/syslogd.pid -x /sbin/syslogd -- -n -C64 -S -D
	start-stop-daemon -S -b -m -p /run/klogd.pid -x /sbin/klogd -- -n
	;;
stop)
	start-stop-daemon -K -p /run/klogd.pid -x /sbin/klogd || true
	start-stop-daemon -K -p /run/syslogd.pid -x /sbin/syslogd || true
	rm -f /run/klogd.pid /run/syslogd.pid
	;;
restart)
	"$0" stop
	"$0" start
	;;
esac
EOF
chmod 0755 "$INITD/S01syslogd"

# A small board-specific login environment. The stock profile probes network
# routes and prints IP state that can never exist on this camera.
cat >"$TARGET_DIR/etc/profile" <<'EOF'
export HOME=/root
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export EDITOR=vi
export PAGER=less
export HISTFILE=/tmp/.ash_history
export HISTSIZE=500
export HISTFILESIZE=500

alias logs='logread'
alias logf='logread -f'
alias uvlog='logread | grep uvcd'
alias uvlogf='logread -f | grep uvcd'
alias controls='uvcdctl list'
alias ll='ls -alF'

if [ -n "$PS1" ]; then
	printf '\033[1;36mCreality K1 USB camera\033[0m  '
	printf 'uvcd: '
	pidof uvcd >/dev/null && printf '\033[1;32mrunning\033[0m\n' || printf '\033[1;31mstopped\033[0m\n'
	printf '  controls   current control values\n'
	printf '  uvlog      uvcd log buffer\n'
	printf '  uvlogf     follow uvcd logs\n'
	printf '  sinfo      sensor information\n\n'
	export PS1='\[\e[38;5;208m\]k1\[\e[0m\]:\[\e[38;5;153m\]\w\[\e[0m\]# '
fi
EOF

cat >"$TARGET_DIR/usr/bin/k1-console" <<'EOF'
#!/bin/sh
export TERM=${TERM:-vt100}
exec /bin/sh -l
EOF
chmod 0755 "$TARGET_DIR/usr/bin/k1-console"

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
# g_webcam, and a getty that fails must not spin. -L ignores carrier; -w waits
# for a clean CR/LF before starting ash, discarding bytes queued while ACM was
# enumerating instead of feeding them to a live shell as commands.
#
# uvcd's supervisor is init's too, so nothing can leave uvcd stopped: the
# gadget, and the ACM console with it, only exists while uvcd runs, so a
# stop typed into that console would otherwise cut the camera off until a
# power cycle. Killed outside shutdown, init starts a new supervisor at once;
# at shutdown it respawns nothing and rcK stops uvcd cleanly.
INITTAB="$TARGET_DIR/etc/inittab"
sed -i '/creality_k1_t31l_gc2083\/post-build.sh/d; /ttyGS0/d; /S31uvcd supervise/d' "$INITTAB"
cat >>"$INITTAB" <<'EOF'
# USB ACM emergency console (added by creality_k1_t31l_gc2083/post-build.sh)
::respawn:/bin/sh -c 'while [ ! -e /dev/ttyGS0 ]; do sleep 1; done; /sbin/getty -L -w -n -l /usr/bin/k1-console ttyGS0 0 vt100; sleep 1'
# uvcd supervisor (added by creality_k1_t31l_gc2083/post-build.sh)
null::respawn:/etc/init.d/S31uvcd supervise
EOF

# Identity. This image is k1cam: built on thingino's tree, not affiliated
# with it, so it must not introduce itself as Thingino. thingino's
# rootfs_script.sh writes its own name, version, CPE, logo and home page
# into os-release; the pass after it replaces those (ID_LIKE credits the
# base). The rest -- build IDs, SoC, the IMAGE_ID the release scripts read --
# stays. Only once that script has run: before it, os-release is still
# Buildroot's, which it re-prefixes with BUILDROOT_.
OS_RELEASE="$TARGET_DIR/usr/lib/os-release"
if grep -q '^ID=thingino$' "$OS_RELEASE" 2>/dev/null; then
	sed -i \
		-e 's/^NAME=Thingino$/NAME=k1cam/' \
		-e 's/^ID=thingino$/ID=k1cam/' \
		-e 's/^PRETTY_NAME=.*/PRETTY_NAME="k1cam"/' \
		-e 's/^ID_LIKE=buildroot$/ID_LIKE="thingino buildroot"/' \
		-e '/^\(VERSION\|VERSION_ID\|VERSION_CODENAME\|CPE_NAME\|LOGO\|HOME_URL\)=/d' \
		"$OS_RELEASE"
fi
printf 'k1cam\n\n' >"$TARGET_DIR/etc/issue"
