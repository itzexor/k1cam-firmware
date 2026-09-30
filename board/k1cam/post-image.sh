#!/bin/sh
# post-image.sh -- pack the full flash image for the K1 camera.
#
# Buildroot runs this last (BR2_ROOTFS_POST_IMAGE_SCRIPT), with BINARIES_DIR
# and HOST_DIR in the environment. The flash layout is fixed and lives in one
# place, the mtdparts line of board/k1cam/uenv.txt: U-Boot passes it to the
# kernel, and this script reads the partition sizes from it. It checks that
# the environment's own kernel and data addresses agree with it and that
# every image fits its partition, then writes:
#
#   u-boot-env.bin  the same environment, for the env partition (U-Boot
#                   also has it built in as its default)
#   data.jffs2      an empty jffs2 overlay filling the data partition
#   k1cam.bin       the whole flash; 0xff wherever nothing is written

set -eu

BOARD_DIR=$(cd "$(dirname "$0")" && pwd)
UENV="$BOARD_DIR/uenv.txt"
ERASE_BLOCK=65536

cd "${BINARIES_DIR:?}"

fail() {
	echo "post-image: $*" >&2
	exit 1
}

env_get() {
	sed -n "s/^$1=//p" "$UENV"
}

# mtdparts=<controller>:<size>k(<name>),...,<size>k@0(all)
parts=$(env_get mtdparts)
[ -n "$parts" ] || fail "no mtdparts in $UENV"
offset=0
for part in $(echo "${parts#*:}" | tr ',' ' '); do
	case "$part" in
		*@*) continue ;; # the whole-flash alias
	esac
	size_kb=${part%%k(*}
	name=${part#*(}
	name=${name%)}
	eval "${name}_offset=$offset ${name}_size=$((size_kb * 1024))"
	offset=$((offset + size_kb * 1024))
done
flash_size=$(($(env_get flash_len)))
[ "$offset" -eq "$flash_size" ] ||
	fail "partitions add up to $offset bytes, flash_len says $flash_size"

same() {
	[ "$(($2))" -eq "$(($3))" ] || fail "$1 is $(($2)), mtdparts says $(($3))"
}
same kern_addr "$(env_get kern_addr)" "$kernel_offset"
same kern_size "$(env_get kern_size)" "$kernel_size"
same data_addr "$(env_get data_addr)" "$data_offset"
same data_size "$(env_get data_size)" "$data_size"

fits() {
	size=$(stat -c %s "$2")
	[ "$size" -le "$3" ] || fail "$2 is $size bytes, the $1 partition $3"
}
uboot=u-boot-with-tpl-lzma.bin
fits boot "$uboot" "$boot_size"
fits kernel uImage "$kernel_size"
fits rootfs rootfs.squashfs "$rootfs_size"

"$HOST_DIR/bin/mkenvimage" -s "$env_size" -o u-boot-env.bin "$UENV"

# The overlay starts out holding just an empty /opt, as thingino's did.
overlay=$(mktemp -d)
trap 'rm -rf "$overlay"' EXIT
mkdir "$overlay/opt"
"$HOST_DIR/sbin/mkfs.jffs2" --little-endian --squash --output=data.jffs2 \
	--root="$overlay" --eraseblock="$ERASE_BLOCK" --pad="$data_size"
fits data data.jffs2 "$data_size"

put() {
	dd if="$1" of=k1cam.bin bs=65536 seek="$2" oflag=seek_bytes conv=notrunc status=none
}
head -c "$flash_size" /dev/zero | tr '\000' '\377' >k1cam.bin
put "$uboot" "$boot_offset"
put u-boot-env.bin "$env_offset"
put uImage "$kernel_offset"
put rootfs.squashfs "$rootfs_offset"
put data.jffs2 "$data_offset"
sha256sum k1cam.bin >k1cam.bin.sha256sum

printf 'post-image: %s\n' "$BINARIES_DIR/k1cam.bin"
for name in boot env backup kernel rootfs data; do
	eval "printf '  %-7s 0x%06x  %5d KiB\n' $name \$${name}_offset \$((\$${name}_size / 1024))"
done
