#!/bin/sh
# image-test.sh -- post-image.sh on the build's own kernel, rootfs and U-Boot:
# the images it writes, checked by other means than it checks them, and the
# layout mistakes it has to refuse.
#
# Needs a k1cam build (output/k1cam): its images and host tools.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
out=${O:-$top/output/k1cam}
board=$here/..

fail() {
	echo "image-test: FAIL: $*" >&2
	exit 1
}

for f in u-boot-with-tpl-lzma.bin uImage rootfs.squashfs; do
	[ -f "$out/images/$f" ] || fail "no $out/images/$f; build k1cam first"
done
[ -x "$out/host/bin/k1cam-mkupd" ] || fail "no k1cam-mkupd; build k1cam first"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/images" "$work/target/usr/lib/k1cam"
cp "$out/images/u-boot-with-tpl-lzma.bin" "$out/images/uImage" "$out/images/rootfs.squashfs" \
	"$work/images/"
cat >"$work/target/usr/lib/k1cam/firmware-release" <<EOF
board=k1cam-t31l-gc2083
layout=k1cam-ab-8m-v1
build=k1cam-image-test
kernel=0123456789ab
min_boot_compat=1
protocol=1
EOF
cat >"$work/target/usr/lib/os-release" <<EOF
NAME=k1cam
BUILD_ID="image-test+0123456, 2026-01-01 00:00:00 +0000"
KERNEL_COMMIT=0123456789ab
EOF
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -I"$top/package/k1cam-uvcd/src" \
	"$top/package/k1cam-uvcd/tests/fwenv_tool.c" -o "$work/fwenv_tool"

# pack [UENV FW_ENV_CONFIG]: post-image.sh as Buildroot runs it.
pack() {
	(BINARIES_DIR=$work/images HOST_DIR=$out/host TARGET_DIR=$work/target \
		K1CAM_UENV=${1:-$board/uenv.txt} \
		K1CAM_FW_ENV_CONFIG=${2:-$board/rootfs-overlay/etc/fw_env.config} \
		sh "$board/post-image.sh") >"$work/log" 2>&1
}

pack || {
	cat "$work/log" >&2
	fail "post-image.sh refused the build's own images"
}
img=$work/images

# k1cam.bin, read independently: each part at its offset, erased after it.
at() { # FILE OFFSET PARTITION_SIZE
	n=$(stat -c %s "$img/$1")
	tail -c +$(($2 + 1)) "$img/k1cam.bin" | head -c "$n" | cmp -s - "$img/$1" ||
		fail "$1 is not at $2"
	[ -z "$(tail -c +$(($2 + n + 1)) "$img/k1cam.bin" | head -c $(($3 - n)) | tr -d '\377' | head -c 1)" ] ||
		fail "not erased after $1 at $2"
}
[ "$(stat -c %s "$img/k1cam.bin")" -eq 8388608 ] || fail "k1cam.bin is not 8 MiB"
at u-boot-with-tpl-lzma.bin 0 0x50000
at uImage 0x70000 0x140000
at rootfs.squashfs 0x1b0000 0x1c0000
at uImage 0x370000 0x140000
at rootfs.squashfs 0x4b0000 0x1c0000
at data.jffs2 0x670000 0x190000
[ "$("$work/fwenv_tool" "$img/k1cam.bin" copies)" = "env_a=valid,1 env_b=valid,0 current=env_a" ] ||
	fail "environment copies: $("$work/fwenv_tool" "$img/k1cam.bin" copies)"
[ "$("$work/fwenv_tool" "$img/k1cam.bin" get active_slot trial_slot)" = "active_slot=a
trial_slot absent" ] || fail "environment state"
(cd "$img" && sha256sum -c --quiet k1cam.bin.sha256sum k1cam.upd.sha256sum) || fail "checksums"

# k1cam.upd, read by the independent parser, and deterministic.
python3 -I "$top/package/k1cam-uvcd/tests/bundle_check.py" "$img/k1cam.upd" >"$work/manifest" ||
	fail "bundle_check.py refused k1cam.upd"
grep -q '^build=k1cam-image-test$' "$work/manifest" || fail "bundle build id"
sed -n 's/^info://p' "$work/manifest" | cmp -s - "$work/target/usr/lib/os-release" ||
	fail "k1cam.upd's release information is not the target's os-release"
cp "$img/k1cam.upd" "$work/first.upd"
pack || fail "second run"
cmp -s "$img/k1cam.upd" "$work/first.upd" || fail "k1cam.upd differs between identical runs"

# Mistakes it must refuse: each a copy of uenv.txt with one thing wrong.
refuse() { # WHAT MESSAGE SED-EXPRESSION [FW_ENV_CONFIG]
	sed "$3" "$board/uenv.txt" >"$work/uenv.txt"
	if pack "$work/uenv.txt" "${4:-}"; then
		fail "$1: accepted"
	fi
	grep -q -- "$2" "$work/log" || {
		cat "$work/log" >&2
		fail "$1: no '$2'"
	}
}
refuse "duplicate partition" "listed twice" \
	's/1600k(data)/1280k(kernel_a),320k(data)/'
refuse "unaligned partition" "not aligned" \
	's/1280k(kernel_a),1792k(rootfs_a)/1248k(kernel_a),1824k(rootfs_a)/'
refuse "sizes not adding up" "add up to" 's/1600k(data)/1536k(data)/'
refuse "layout unlike the updater's" "mtdparts has" \
	's/1280k(kernel_a),1792k(rootfs_a)/1216k(kernel_a),1856k(rootfs_a)/'
refuse "partition the updater lacks" "updater" 's/320k(boot)/256k(boot),64k(spl)/'
refuse "no whole-flash alias" "lacks the whole-flash alias" 's/,8192k@0(all)//'
refuse "malformed address" "is not a number" 's/^kernel_a_addr=.*/kernel_a_addr=0x7g000/'
refuse "wrong address" "kernel_b_addr is" 's/^kernel_b_addr=.*/kernel_b_addr=0x380000/'
refuse "wrong root device" "rootfs_b_dev" 's/^rootfs_b_dev=.*/rootfs_b_dev=\/dev\/mtdblock5/'
refuse "wrong layout id" "k1_layout" 's/^k1_layout=.*/k1_layout=k1cam-ab-8m-v2/'
refuse "variable set twice" "set more than once" 's/^bootlimit=3$/bootlimit=3\nbootlimit=5/'
printf '/dev/mtd1 0x0 0x10000 0x8000\n' >"$work/fw_env.config"
refuse "one-copy fw_env.config" "fw_env.config is not" 's/x/x/' "$work/fw_env.config"

# Images that don't fit.
head -c 1310721 /dev/zero >"$img/uImage"
pack && fail "oversized kernel: accepted"
grep -q "the kernel_a partition" "$work/log" || fail "oversized kernel: wrong reason"

echo "image-test: k1cam.bin, k1cam.upd and every refusal check out"
