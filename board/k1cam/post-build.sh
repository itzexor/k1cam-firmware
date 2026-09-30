#!/bin/sh
# Final target fixups that cannot be expressed as ordinary overlay files.

set -e

TARGET_DIR=${1:?usage: post-build.sh TARGET_DIR}
EXTERNAL_DIR=$(CDPATH= cd "$(dirname "$0")/../.." && pwd)

# Some Ingenic binaries request the historical combined-uClibc sonames.
if [ -L "$TARGET_DIR/lib" ] || [ ! -d "$TARGET_DIR/lib" ]; then
	LIB_DIR="$TARGET_DIR/usr/lib"
else
	LIB_DIR="$TARGET_DIR/lib"
fi
for libuclibc in "$LIB_DIR"/libuClibc-*.so; do
	[ -e "$libuclibc" ] || continue
	ln -srf "$libuclibc" "$LIB_DIR/libpthread.so.0"
	ln -srf "$libuclibc" "$LIB_DIR/libdl.so.0"
	ln -srf "$libuclibc" "$LIB_DIR/libm.so.0"
	break
done

# Mount points /init needs before it can pivot onto the writable overlay.
# Git cannot carry empty directories in rootfs-overlay.
mkdir -p "$TARGET_DIR/overlay" "$TARGET_DIR/rom"

cat >"$TARGET_DIR/usr/bin/ldd" <<'EOF'
#!/bin/sh
LD_TRACE_LOADED_OBJECTS=1 exec "$@"
EOF
chmod 0755 "$TARGET_DIR/usr/bin/ldd"
rm -f "$LIB_DIR"/libstdc++.so* "$LIB_DIR"/libstdc++.so.6.0.*-gdb.py

# Preserve Buildroot's os-release fields as provenance, then add the fixed
# product identity and the source revision used for this image.
OS_RELEASE="$TARGET_DIR/usr/lib/os-release"
BUILDROOT_RELEASE=$(mktemp)
if grep -q '^ID=k1cam$' "$OS_RELEASE" 2>/dev/null; then
	sed -n 's/^BUILDROOT_//p' "$OS_RELEASE" | sed 's/^/BUILDROOT_/' >"$BUILDROOT_RELEASE"
else
	sed 's/^/BUILDROOT_/' "$OS_RELEASE" >"$BUILDROOT_RELEASE"
fi
GIT_BRANCH=$(git -C "$EXTERNAL_DIR" symbolic-ref --quiet --short HEAD 2>/dev/null || printf detached)
GIT_HASH=$(git -C "$EXTERNAL_DIR" show -s --format=%H)
GIT_SHORT_HASH=$(printf '%.7s' "$GIT_HASH")
# Say so when the image was built from uncommitted changes to tracked files.
git -C "$EXTERNAL_DIR" diff --quiet HEAD -- 2>/dev/null || GIT_SHORT_HASH="$GIT_SHORT_HASH-dirty"
KERNEL_COMMIT=$(sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION="\(.*\)"$/\1/p' "${BR2_CONFIG:?}")
GIT_TIME=$(TZ=UTC0 git -C "$EXTERNAL_DIR" show -s --date='format-local:%Y-%m-%d %H:%M:%S +0000' --format=%cd)
BUILD_TIME=$(env -u SOURCE_DATE_EPOCH TZ=UTC date '+%Y-%m-%d %H:%M:%S %z')
BUILD_EPOCH=$(date +%s)
cat >"$OS_RELEASE" <<EOF
NAME=k1cam
ID=k1cam
PRETTY_NAME="k1cam"
ID_LIKE=buildroot
ANSI_COLOR="1;34"
ARCHITECTURE=mips
LIBC=uclibc
TOOLCHAIN=uclibc
TOOLCHAIN_TYPE=external
TOOLCHAIN_GCC=16
SOC=t31
SOC_ARCH=xburst1
IMAGE_ID=k1cam
IMAGE_NAME="Creality K1 USB camera"
BUILD_ID="$GIT_BRANCH+$GIT_SHORT_HASH, $BUILD_TIME"
BUILD_TIME="$BUILD_TIME"
COMMIT_ID="$GIT_BRANCH+$GIT_SHORT_HASH, $GIT_TIME"
BOOTLOADER=isvp_t31l_sfcnor
KERNEL_COMMIT=$KERNEL_COMMIT
HOSTNAME=k1cam
TIME_STAMP=$BUILD_EPOCH
EOF
cat "$BUILDROOT_RELEASE" >>"$OS_RELEASE"
rm -f "$BUILDROOT_RELEASE"

"$EXTERNAL_DIR/scripts/check-busybox-lopts.sh" "$TARGET_DIR" 1
