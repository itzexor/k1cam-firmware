#!/bin/sh
# run.sh -- build and run uvcd's host tests under ASan and UBSan.
#
# They include uvcd's sources directly and need the raptor-hal headers from a
# k1cam build, so run `make` first. O selects another output directory.
#
# The firmware tests also use the build's own mkenvimage (U-Boot's) to make
# environment fixtures, and its uImage and rootfs.squashfs when present.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../src" && pwd)
top=$(cd "$here/../../.." && pwd)
out=${O:-$top/output/k1cam}

hal=$(find "$out/build" -maxdepth 3 -name raptor_hal.h -path '*thingino-raptor-hal*' 2>/dev/null | head -1)
if [ -z "$hal" ]; then
	echo "run.sh: no raptor_hal.h under $out/build; build first" >&2
	exit 1
fi

bin=$(mktemp -d)
trap 'rm -rf "$bin"' EXIT
fixtures=$bin/fixtures
mkdir "$fixtures"
if [ -x "$out/host/bin/mkenvimage" ]; then
	printf '%s\n' 'active_slot=a' 'bootlimit=3' \
		'k1_boot=if test x = x; then echo yes; fi' >"$bin/env.txt"
	"$out/host/bin/mkenvimage" -r -s 0x10000 -o "$fixtures/env-redund.bin" "$bin/env.txt"
fi
for image in uImage rootfs.squashfs; do
	[ ! -f "$out/images/$image" ] || cp "$out/images/$image" "$fixtures/"
done

# The bundle format's golden vector, read independently of the C code.
golden=$(python3 -I "$here/bundle_check.py" --golden)
grep -q "\"$golden\"" "$here/fw_test.c" || {
	echo "run.sh: bundle_check.py golden vector $golden is not the one fw_test.c pins" >&2
	exit 1
}
if [ -f "$out/images/k1cam.upd" ]; then
	python3 -I "$here/bundle_check.py" "$out/images/k1cam.upd" >/dev/null
fi

firmware="$src/uvcd_update.c $src/fw_bundle.c $src/fw_crc.c $src/fw_env.c $src/fw_flash.c $src/fw_sha256.c"
set -- -std=gnu11 -D_GNU_SOURCE -g -O1 -Wall -Wextra \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-ffunction-sections -fdata-sections -I"$(dirname "$hal")" -I"$src"
for test in config fw update gadget pipeline startup uvcdctl; do
	case $test in
		gadget | uvcdctl) extra=$firmware ;;
		*) extra= ;;
	esac
	# shellcheck disable=SC2086
	${CC:-cc} "$@" "$here/${test}_test.c" $extra -o "$bin/$test" -Wl,--gc-sections -lpthread -lm
	(cd "$bin" && TMPDIR=$bin FW_TEST_FIXTURES=$fixtures "./$test")
done
