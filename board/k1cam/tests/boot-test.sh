#!/bin/sh
# boot-test.sh -- the A/B boot logic in board/k1cam/uenv.txt, run by U-Boot.
#
# U-Boot's sandbox build (from the same U-Boot release as the camera's, out
# of dl/) runs the environment's scripts with the camera's environment
# setup: redundant copies at 0x50000 and 0x60000 of an emulated 8 MiB SPI
# NOR, the same env/sf.c code, the old hush parser, uenv.txt as the default
# environment. Booting a kernel is replaced through k1_bootm, which reports
# the slot and stops; the boot decisions and environment writes are real.
# uvcd's environment code (fwenv_tool) reads and writes the same images, so
# U-Boot and Linux are checked against each other.
#
# Needs a k1cam build (for mkenvimage) and builds the sandbox into
# output/uboot-sandbox the first time, which takes a few minutes.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
out=${O:-$top/output/k1cam}
sandbox=$top/output/uboot-sandbox
uenv=$here/../uenv.txt
mkenvimage=$out/host/bin/mkenvimage

fail() {
	echo "boot-test: FAIL: $*" >&2
	exit 1
}

[ -x "$mkenvimage" ] || fail "no $mkenvimage; build k1cam first"

version=$(sed -n 's/^BR2_TARGET_UBOOT_CUSTOM_VERSION_VALUE="\(.*\)"$/\1/p' \
	"$top/configs/k1cam_defconfig")
src=$sandbox/u-boot-$version

if [ ! -f "$src/.k1cam-configured" ]; then
	tarball=$top/dl/uboot/u-boot-$version.tar.bz2
	[ -f "$tarball" ] || fail "no $tarball"
	rm -rf "$src"
	mkdir -p "$sandbox"
	tar -C "$sandbox" -xjf "$tarball"
	# The host's swig needs this, as for the camera's U-Boot.
	patch -d "$src" -p1 -s <"$top/package/all-patches/uboot/0003-pylibfdt-fix-build-with-swig-4.5.patch"
	# An 8 MiB flash like the camera's, and the environment only there.
	# and, as there, no watchdog until a script starts one.
	sed -i -e 's/"spansion,m25p16", "jedec,spi-nor"/"winbond,w25q64cv", "jedec,spi-nor"/' \
		-e '/u-boot,autostart;/d' "$src/arch/sandbox/dts/sandbox.dtsi"
	python3 -I - "$src/board/sandbox/sandbox.c" <<'EOF'
import sys
path = sys.argv[1]
text = open(path).read()
old = """static enum env_location env_locations[] = {
	ENVL_NOWHERE,
	ENVL_EXT4,
	ENVL_FAT,
	ENVL_SPI_FLASH,
};"""
assert old in text
open(path, "w").write(text.replace(old, """static enum env_location env_locations[] = {
	ENVL_SPI_FLASH,
};"""))
EOF
	make -C "$src" -s sandbox_defconfig
	cat >"$src/k1cam.fragment" <<EOF
# CONFIG_ENV_IS_NOWHERE is not set
# CONFIG_ENV_IS_IN_FAT is not set
# CONFIG_ENV_IS_IN_EXT4 is not set
CONFIG_ENV_IS_IN_SPI_FLASH=y
# CONFIG_ENV_SECT_SIZE_AUTO is not set
CONFIG_ENV_SIZE=0x10000
CONFIG_ENV_OFFSET=0x50000
CONFIG_ENV_SECT_SIZE=0x10000
CONFIG_ENV_REDUNDANT=y
CONFIG_ENV_OFFSET_REDUND=0x60000
# CONFIG_ENV_REDUNDANT_UPGRADE is not set
CONFIG_ENV_USE_DEFAULT_ENV_TEXT_FILE=y
CONFIG_ENV_DEFAULT_ENV_TEXT_FILE="$uenv"
# CONFIG_BOOTCOUNT_LIMIT is not set
CONFIG_HUSH_PARSER=y
CONFIG_HUSH_OLD_PARSER=y
# CONFIG_HUSH_MODERN_PARSER is not set
CONFIG_CMD_WDT=y
# CONFIG_WATCHDOG_AUTOSTART is not set
CONFIG_CMD_ITEST=y
CONFIG_CMD_SETEXPR=y
# CONFIG_SANDBOX_SDL is not set
# CONFIG_EFI_LOADER is not set
# CONFIG_UNIT_TEST is not set
# CONFIG_CMD_UPL is not set
# CONFIG_UPL is not set
EOF
	(cd "$src" && ./scripts/kconfig/merge_config.sh -m .config k1cam.fragment >/dev/null)
	make -C "$src" -s olddefconfig
	touch "$src/.k1cam-configured"
fi
# Incremental: picks up a changed uenv.txt.
make -C "$src" -s -j"$(nproc)" NO_SDL=1 u-boot dtbs >"$sandbox/build.log" 2>&1 ||
	fail "sandbox build failed; see $sandbox/build.log"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tool=$work/fwenv_tool
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -Wall -Wextra \
	-I"$top/package/k1cam-uvcd/src" "$top/package/k1cam-uvcd/tests/fwenv_tool.c" -o "$tool"

# A flash as post-image.sh writes it: both environment copies from uenv.txt
# (env_a active, env_b obsolete), with NAME=VALUE overrides (empty: none).
image() {
	cp "$uenv" "$work/env.txt"
	for kv in "$@"; do
		name=${kv%%=*}
		grep -v "^$name=" "$work/env.txt" >"$work/env.new" || true
		[ -z "${kv#*=}" ] || printf '%s\n' "$kv" >>"$work/env.new"
		mv "$work/env.new" "$work/env.txt"
	done
	"$mkenvimage" -r -s 0x10000 -o "$work/env_a.bin" "$work/env.txt"
	cp "$work/env_a.bin" "$work/env_b.bin"
	printf '\000' | dd of="$work/env_b.bin" bs=1 seek=4 conv=notrunc status=none
	head -c 8388608 /dev/zero | tr '\000' '\377' >"$work/spi.bin"
	dd if="$work/env_a.bin" of="$work/spi.bin" bs=65536 seek=5 conv=notrunc status=none
	dd if="$work/env_b.bin" of="$work/spi.bin" bs=65536 seek=6 conv=notrunc status=none
}

# The camera's k1_bootm boots the kernel; this one reports what would boot,
# and fails instead for the slot named by t_bad (or every slot: all).
# shellcheck disable=SC2016
mock='setenv loadaddr 0x1000000; setenv k1_wdt_dev alarm-wdt;
setenv k1_bootm '\''if test "${k1_slot}" = "${t_bad}" || test "${t_bad}" = all; then echo K1FAIL ${k1_slot}; false; else echo K1BOOT ${k1_slot} ${k1_mode} ${k1_kaddr}; echo K1ARGS ${bootargs}; poweroff; fi'\''; '

# boot [COMMANDS]: one power-on, COMMANDS run before bootcmd.
boot() {
	cp "$work/spi.bin" "$work/before.bin"
	(cd "$work" && timeout 60 "$src/u-boot" -d "$src/arch/sandbox/dts/sandbox.dtb" \
		-c "setenv t_bad none; $mock ${1:-} run bootcmd; echo SCRIPT-ENDED") \
		>"$work/out" 2>&1 || true
}

expect() {
	grep -q -- "$1" "$work/out" || {
		cat "$work/out" >&2
		fail "$2: no '$1' in the output"
	}
}

reject() {
	if grep -q -- "$1" "$work/out"; then
		cat "$work/out" >&2
		fail "$2: '$1' in the output"
	fi
}

env_is() {
	got=$("$tool" "$work/spi.bin" get "$1")
	[ "$got" = "$2" ] || fail "$3: $1: got '$got', want '$2'"
}

unchanged() {
	cmp -s "$work/before.bin" "$work/spi.bin" || fail "$1: the boot wrote the flash"
}

copies() {
	got=$("$tool" "$work/spi.bin" copies)
	[ "$got" = "$1" ] || fail "$2: copies: got '$got', want '$1'"
}

# Normal boots of either slot read the environment and write nothing.
image
copies "env_a=valid,1 env_b=valid,0 current=env_a" "installed image"
boot
expect "K1BOOT a confirmed 0x70000" "slot a"
expect "root=/dev/mtdblock4 rootfstype=squashfs init=/init mtdparts=jz_sfc:320k(boot),64k(env_a)" "slot a args"
expect "k1cam.slot=a k1cam.boot=confirmed k1cam.layout=k1cam-ab-8m-v1" "slot a args"
reject "Started alarm-wdt" "no watchdog for a confirmed boot"
unchanged "slot a"
image active_slot=b
boot
expect "K1BOOT b confirmed 0x370000" "slot b"
expect "root=/dev/mtdblock6" "slot b args"
unchanged "slot b"

# A trial: each boot is counted on flash first, under the watchdog;
# after bootlimit boots the confirmed slot is back and the trial gone.
image upgrade_available=1 trial_slot=b
for n in 1 2 3; do
	boot
	expect "trial boot $n of 3 of slot b" "trial $n"
	expect "Started alarm-wdt.*(120s timeout)" "trial $n watchdog"
	expect "K1BOOT b trial 0x370000" "trial $n"
	expect "k1cam.slot=b k1cam.boot=trial" "trial $n args"
	env_is bootcount "bootcount=$n" "trial $n"
	env_is trial_slot "trial_slot=b" "trial $n"
done
copies "env_a=valid,0 env_b=valid,1 current=env_b" "after three saves"
boot
expect "trial slot b failed 3 boots" "exhausted trial"
expect "K1BOOT a confirmed" "back to slot a"
env_is trial_slot "trial_slot absent" "exhausted trial"
env_is upgrade_available "upgrade_available=0" "exhausted trial"
env_is bootcount "bootcount=0" "exhausted trial"
env_is active_slot "active_slot=a" "exhausted trial"
env_is k1_slot "k1_slot absent" "temporaries not saved"
boot
expect "K1BOOT a confirmed" "after the rollback"
unchanged "after the rollback"

# A trial kernel that won't load goes back to the confirmed slot at once.
image upgrade_available=1 trial_slot=b
boot "setenv t_bad b;"
expect "K1FAIL b" "bad trial kernel"
expect "dropping trial slot b, back to slot a" "bad trial kernel"
expect "K1BOOT a confirmed" "bad trial kernel"
env_is trial_slot "trial_slot absent" "bad trial kernel"

# If the attempt can't be recorded, the trial doesn't run.
image upgrade_available=1 trial_slot=b
boot "setenv k1_save false;"
expect "cannot record the trial boot" "unsaved attempt"
expect "K1BOOT a confirmed" "unsaved attempt"
reject "K1BOOT b" "unsaved attempt"
unchanged "unsaved attempt"

# Contradictory or broken state: the confirmed slot, and no loop.
image upgrade_available=1 trial_slot=c
boot
expect "trial_slot (c) is not a or b" "bad trial slot"
expect "K1BOOT a confirmed" "bad trial slot"
env_is upgrade_available "upgrade_available=0" "bad trial slot"
image upgrade_available=1 trial_slot=a
boot
expect "trial slot a is the confirmed slot" "trial of the confirmed slot"
expect "K1BOOT a confirmed" "trial of the confirmed slot"
image upgrade_available=1
boot
expect "trial_slot () is not a or b" "trial without a slot"
expect "K1BOOT a confirmed" "trial without a slot"
image upgrade_available=1 trial_slot=b bootlimit=
boot
expect "failed  boots" "no bootlimit"
expect "K1BOOT a confirmed" "no bootlimit"
image upgrade_available=0 trial_slot=b
boot
expect "K1BOOT a confirmed" "trial not scheduled"
unchanged "trial not scheduled"
image active_slot=x
boot
expect "active_slot (x) is not a or b; using a" "bad active slot"
expect "K1BOOT a confirmed" "bad active slot"

# A confirmed slot that won't load: the other slot, under the watchdog.
image
boot "setenv t_bad a;"
expect "K1FAIL a" "bad confirmed kernel"
expect "Started alarm-wdt.*(120s timeout)" "fallback watchdog"
expect "K1BOOT b fallback 0x370000" "bad confirmed kernel"
expect "k1cam.boot=fallback" "fallback args"
unchanged "fallback"
boot "setenv t_bad all;"
expect "no slot booted" "nothing bootable"

# Linux (uvcd) schedules a trial; U-Boot runs it; Linux confirms it.
image
"$tool" "$work/spi.bin" set trial_slot=b upgrade_available=1 bootcount=0
copies "env_a=valid,0 env_b=valid,1 current=env_b" "scheduled by Linux"
boot
expect "K1BOOT b trial" "trial scheduled by Linux"
copies "env_a=valid,1 env_b=valid,0 current=env_a" "counted by U-Boot"
env_is bootcount "bootcount=1" "counted by U-Boot"
"$tool" "$work/spi.bin" set active_slot=b trial_slot= upgrade_available=0 bootcount=0
boot
expect "K1BOOT b confirmed" "confirmed by Linux"
unchanged "confirmed by Linux"

# One copy damaged: the other one is used; both gone: uenv.txt's defaults.
image
"$tool" "$work/spi.bin" set active_slot=b
printf 'X' | dd of="$work/spi.bin" bs=1 seek=$((0x60010)) conv=notrunc status=none
boot
expect "K1BOOT a confirmed" "newer copy damaged: the older one"
image
"$tool" "$work/spi.bin" set active_slot=b
printf 'X' | dd of="$work/spi.bin" bs=1 seek=$((0x50010)) conv=notrunc status=none
boot
expect "K1BOOT b confirmed" "older copy damaged: the newer one"
image upgrade_available=1 trial_slot=b
head -c 131072 /dev/zero | tr '\000' '\377' |
	dd of="$work/spi.bin" bs=65536 seek=5 conv=notrunc status=none
boot
expect "K1BOOT a confirmed" "no environment: the built-in default"

echo "boot-test: all boot scenarios passed"
