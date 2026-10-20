#!/bin/sh
# post-image.sh -- pack the K1 camera's flash image and its update bundle.
#
# Buildroot runs this last (BR2_ROOTFS_POST_IMAGE_SCRIPT), with BINARIES_DIR,
# HOST_DIR and TARGET_DIR in the environment. The flash layout is written
# down twice: for U-Boot and Linux in the mtdparts line of
# board/k1cam/uenv.txt, and for the updater in fw_layout.h, compiled into uvcd
# and k1cam-mkupd. This script refuses to pack when they disagree, or when
# the environment's addresses and devices, or /etc/fw_env.config, don't match
# the partitions. It writes:
#
#   u-boot-env.bin    the environment for env_a, marked active
#   u-boot-env-b.bin  the same for env_b, marked obsolete: U-Boot reads env_a
#                     and has an intact second copy from the start
#   data.jffs2        the shared data partition: /opt and each slot's
#                     settings layer (slots/a, slots/b), all empty
#   k1cam.bin         the whole flash, both slots holding this build: an
#                     installation, which also resets the settings
#   k1cam.upd         the routine update: this kernel and rootfs only
#   *.sha256sum       their checksums
#   k1cam-layout.txt  the layout, for people

set -eu

BOARD_DIR=$(cd "$(dirname "$0")" && pwd)
# board/k1cam/tests/image-test.sh points these at broken copies.
UENV=${K1CAM_UENV:-$BOARD_DIR/uenv.txt}
FW_ENV_CONFIG=${K1CAM_FW_ENV_CONFIG:-$BOARD_DIR/rootfs-overlay/etc/fw_env.config}
RELEASE="${TARGET_DIR:?}/usr/lib/k1cam/firmware-release"
MKUPD="${HOST_DIR:?}/bin/k1cam-mkupd"
# The SFC NOR driver erases in 32 KiB blocks (mtd erasesize), not 64 KiB.
ERASE_BLOCK=32768
ALIGN=65536

cd "${BINARIES_DIR:?}"

fail() {
	echo "post-image: $*" >&2
	exit 1
}

env_get() {
	n=$(grep -c "^$1=" "$UENV" || true)
	[ "$n" -eq 1 ] || fail "$1 is set $n times in $UENV"
	sed -n "s/^$1=//p" "$UENV"
}

# A number written as decimal or 0x hex, nothing else.
number() {
	case "$1" in
		0x*) v=${1#0x} ;;
		*) v=$1 ;;
	esac
	case "$v" in
		"" | *[!0-9a-fA-F]*) return 1 ;;
	esac
	case "$1" in
		0x*) ;;
		*[!0-9]*) return 1 ;;
	esac
	return 0
}

same() {
	number "$2" || fail "$1: '$2' is not a number"
	[ "$(($2))" -eq "$(($3))" ] || fail "$1 is $2, the layout says $(printf '0x%x' "$3")"
}

# A name set twice would leave U-Boot taking the last one: refuse.
dups=$(sed -n 's/=.*//p' "$UENV" | sort | uniq -d | tr '\n' ' ')
[ -z "$dups" ] || fail "set more than once in $UENV: $dups"

# mtdparts=jz_sfc:<size>k(<name>),...,<size>k@0(all), contiguous from 0.
parts=$(env_get mtdparts)
case "$parts" in
	jz_sfc:*) ;;
	*) fail "mtdparts does not start with jz_sfc:" ;;
esac
flash_size=$(env_get flash_len)
number "$flash_size" || fail "flash_len '$flash_size' is not a number"
flash_size=$((flash_size))
offset=0
index=0
names=" "
all=
for part in $(printf '%s' "${parts#jz_sfc:}" | tr ',' ' '); do
	[ -z "$all" ] || fail "mtdparts has partitions after the whole-flash alias"
	case "$part" in
		*k@0\(all\))
			all=${part%k@0(all)}
			continue
			;;
		*k\(*\)) ;;
		*) fail "bad mtdparts entry '$part'" ;;
	esac
	size_kb=${part%%k(*}
	name=${part#*(}
	name=${name%)}
	case "$size_kb" in
		"" | *[!0-9]*) fail "bad size in '$part'" ;;
	esac
	case "$name" in
		"" | *[!a-z0-9_]*) fail "bad name in '$part'" ;;
	esac
	case "$names" in
		*" $name "*) fail "partition $name is listed twice" ;;
	esac
	size=$((size_kb * 1024))
	[ $((offset % ALIGN)) -eq 0 ] && [ $((size % ALIGN)) -eq 0 ] ||
		fail "$name is not aligned to 64 KiB"
	names="$names$name "
	eval "${name}_offset=$offset ${name}_size=$size ${name}_index=$index"
	offset=$((offset + size))
	index=$((index + 1))
done
[ -n "$all" ] || fail "mtdparts lacks the whole-flash alias"
[ $((all * 1024)) -eq "$flash_size" ] || fail "the whole-flash alias is ${all}k"
[ "$offset" -eq "$flash_size" ] ||
	fail "partitions add up to $offset bytes, flash_len says $flash_size"

# The same layout as the updater's compiled table, entry for entry.
layout=$("$MKUPD" --layout)
seen=0
while read -r key a b; do
	case "$key" in
		id) layout_id=$a ;;
		board) board_id=$a ;;
		boot_compat) boot_compat=$a ;;
		all) [ "$((a))" -eq 0 ] && [ "$((b))" -eq "$flash_size" ] || fail "updater flash size $b" ;;
		*)
			case "$names" in
				*" $key "*) ;;
				*) fail "the updater has a partition $key that mtdparts lacks" ;;
			esac
			eval "o=\$${key}_offset s=\$${key}_size"
			[ "$((a))" -eq "$o" ] && [ "$((b))" -eq "$s" ] ||
				fail "$key: mtdparts has $o+$s, the updater $((a))+$((b))"
			seen=$((seen + 1))
			;;
	esac
done <<EOF
$layout
EOF
[ "$seen" -eq "$index" ] || fail "mtdparts has $index partitions, the updater $seen"

# The environment's own view of the layout.
same kernel_a_addr "$(env_get kernel_a_addr)" "$kernel_a_offset"
same kernel_b_addr "$(env_get kernel_b_addr)" "$kernel_b_offset"
same kernel_size "$(env_get kernel_size)" "$kernel_a_size"
same kernel_size "$(env_get kernel_size)" "$kernel_b_size"
[ "$rootfs_a_size" -eq "$rootfs_b_size" ] || fail "rootfs_a and rootfs_b differ in size"
same data_addr "$(env_get data_addr)" "$data_offset"
same data_size "$(env_get data_size)" "$data_size"
[ "$(env_get rootfs_a_dev)" = "/dev/mtdblock$rootfs_a_index" ] ||
	fail "rootfs_a_dev is not /dev/mtdblock$rootfs_a_index"
[ "$(env_get rootfs_b_dev)" = "/dev/mtdblock$rootfs_b_index" ] ||
	fail "rootfs_b_dev is not /dev/mtdblock$rootfs_b_index"
[ "$(env_get k1_layout)" = "$layout_id" ] || fail "k1_layout is not $layout_id"
[ "$(env_get k1_boot_compat)" = "$boot_compat" ] || fail "k1_boot_compat is not $boot_compat"
[ "$env_a_size" -eq "$env_b_size" ] || fail "env_a and env_b differ in size"
# fw_printenv's view: both copies, by their MTD numbers.
want="/dev/mtd$env_a_index 0x0 $(printf '0x%x' "$env_a_size") $(printf '0x%x' "$ERASE_BLOCK")
/dev/mtd$env_b_index 0x0 $(printf '0x%x' "$env_b_size") $(printf '0x%x' "$ERASE_BLOCK")"
[ "$(grep -v '^#' "$FW_ENV_CONFIG")" = "$want" ] || fail "$FW_ENV_CONFIG is not:
$want"

# The build's identity, as the camera will report it.
release() {
	v=$(sed -n "s/^$1=//p" "$RELEASE")
	[ -n "$v" ] || fail "no $1 in $RELEASE"
	printf '%s' "$v"
}
[ "$(release board)" = "$board_id" ] || fail "$RELEASE board is not $board_id"
[ "$(release layout)" = "$layout_id" ] || fail "$RELEASE layout is not $layout_id"
build=$(release build)
min_boot_compat=$(release min_boot_compat)
[ "$min_boot_compat" -le "$boot_compat" ] ||
	fail "this release needs boot compatibility $min_boot_compat, the environment has $boot_compat"

fits() {
	size=$(stat -c %s "$2")
	[ "$size" -le "$3" ] || fail "$2 is $size bytes, the $1 partition $3"
}
uboot=u-boot-with-tpl-lzma.bin
fits boot "$uboot" "$boot_size"
fits kernel_a uImage "$kernel_a_size"
fits rootfs_a rootfs.squashfs "$rootfs_a_size"

# env_a active (1), env_b the same, obsolete (0).
"$HOST_DIR/bin/mkenvimage" -r -s "$env_a_size" -o u-boot-env.bin "$UENV"
cp u-boot-env.bin u-boot-env-b.bin
printf '\000' | dd of=u-boot-env-b.bin bs=1 seek=4 conv=notrunc status=none
flags=$(od -An -tx1 -j4 -N1 u-boot-env.bin)$(od -An -tx1 -j4 -N1 u-boot-env-b.bin)
[ "$(echo $flags)" = "01 00" ] || fail "environment flags are $flags, not 01 00"
cmp -s -i 5 u-boot-env.bin u-boot-env-b.bin || fail "the environment copies differ"

# The shared data partition, with each slot's settings layer.
data=$(mktemp -d)
trap 'rm -rf "$data"' EXIT
mkdir -p "$data/opt" "$data/slots/a/root" "$data/slots/a/work" \
	"$data/slots/b/root" "$data/slots/b/work"
"$HOST_DIR/sbin/mkfs.jffs2" --little-endian --squash --output=data.jffs2 \
	--root="$data" --eraseblock="$ERASE_BLOCK" --pad="$data_size"
fits data data.jffs2 "$data_size"

# The whole flash: 0xff wherever nothing is written, both slots this build.
put() {
	eval "o=\$${1}_offset s=\$${1}_size"
	dd if="$2" of=k1cam.bin bs=65536 seek="$o" oflag=seek_bytes conv=notrunc status=none
	# Read back: the file, then erased flash to the end of the partition.
	n=$(stat -c %s "$2")
	tail -c +$((o + 1)) k1cam.bin | head -c "$n" | cmp -s - "$2" ||
		fail "$2 does not read back from k1cam.bin at $o"
	[ -z "$(tail -c +$((o + n + 1)) k1cam.bin | head -c $((s - n)) | tr -d '\377' | head -c 1)" ] ||
		fail "$1 is not erased after $2"
}
head -c "$flash_size" /dev/zero | tr '\000' '\377' >k1cam.bin
put boot "$uboot"
put env_a u-boot-env.bin
put env_b u-boot-env-b.bin
put kernel_a uImage
put rootfs_a rootfs.squashfs
put kernel_b uImage
put rootfs_b rootfs.squashfs
put data data.jffs2
[ "$(stat -c %s k1cam.bin)" -eq "$flash_size" ] || fail "k1cam.bin is not $flash_size bytes"
sha256sum k1cam.bin >k1cam.bin.sha256sum

# The update bundle, parsed back the way the camera will parse it.
"$MKUPD" -o k1cam.upd -k uImage -r rootfs.squashfs -B "$build" -c "$min_boot_compat"
check=$("$MKUPD" --check k1cam.upd)
for c in kernel:uImage rootfs:rootfs.squashfs; do
	digest=$(sha256sum "${c#*:}" | cut -d' ' -f1)
	printf '%s\n' "$check" | grep -q "^${c%%:*}=.* sha256:$digest$" ||
		fail "k1cam.upd's ${c%%:*} is not ${c#*:}"
done
sha256sum k1cam.upd >k1cam.upd.sha256sum

{
	printf 'k1cam flash layout %s, build %s\n\n' "$layout_id" "$build"
	printf '%-9s %-8s %-8s %s\n' name offset size device
	for name in $names; do
		eval "o=\$${name}_offset s=\$${name}_size i=\$${name}_index"
		printf '%-9s 0x%06x %5d KiB  /dev/mtd%d\n' "$name" "$o" $((s / 1024)) "$i"
	done
} >k1cam-layout.txt

printf 'post-image: %s\n' "$BINARIES_DIR/k1cam.bin"
printf 'post-image: %s (%s)\n' "$BINARIES_DIR/k1cam.upd" "$build"
tail -n +3 k1cam-layout.txt | sed 's/^/  /'
