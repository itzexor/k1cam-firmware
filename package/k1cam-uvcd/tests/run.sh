#!/bin/sh
# run.sh -- build and run uvcd's host tests under ASan and UBSan.
#
# They include uvcd's sources directly and need the raptor-hal headers from a
# k1cam build, so run `make` first. O selects another output directory.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../src" && pwd)
top=$(cd "$here/../../.." && pwd)
out=${O:-$top/output/k1cam}

hal=$(find "$out/build" -maxdepth 3 -name raptor_hal.h -path '*/thingino-raptor-hal*' 2>/dev/null | head -1)
if [ -z "$hal" ]; then
	echo "run.sh: no raptor_hal.h under $out/build; build first" >&2
	exit 1
fi

bin=$(mktemp -d)
trap 'rm -rf "$bin"' EXIT
set -- -std=gnu11 -D_GNU_SOURCE -g -O1 -Wall -Wextra \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-ffunction-sections -fdata-sections -I"$(dirname "$hal")" -I"$src"
for test in config gadget pipeline startup; do
	${CC:-cc} "$@" "$here/${test}_test.c" -o "$bin/$test" -Wl,--gc-sections -lpthread -lm
	"$bin/$test"
done
