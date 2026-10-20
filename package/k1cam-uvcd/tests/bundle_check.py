#!/usr/bin/env python3
"""bundle_check.py -- an independent reading of the k1cam.upd format.

Written from the format description in src/fw_bundle.h, not from the C
code, so the two implementations check each other:

  bundle_check.py FILE      validate a bundle, print its manifest
  bundle_check.py --golden  build the test bundle fw_test.c builds and
                            print its SHA-256 (the frozen golden vector)

Standard library only. Exit status 0 when valid.
"""

import hashlib
import struct
import sys
import zlib

MAGIC = b"K1CAMUPD"
HEADER_SIZE = 256
RECORD_SIZE = 64
MANIFEST_SIZE = HEADER_SIZE + 2 * RECORD_SIZE
ALIGN = 64
KERNEL_CAPACITY = 0x140000
ROOTFS_CAPACITY = 0x1C0000
DIGEST_AT = 0x0C0


class Invalid(Exception):
    pass


def manifest_digest(buf):
    m = bytearray(buf[:MANIFEST_SIZE])
    m[DIGEST_AT:DIGEST_AT + 32] = bytes(32)
    return hashlib.sha256(m).digest()


def text_field(raw):
    if b"\0" not in raw or raw[0] == 0:
        raise Invalid("identifier not NUL-terminated or empty")
    end = raw.index(b"\0")
    if any(raw[end:]):
        raise Invalid("identifier padding not zero")
    s = raw[:end]
    if any(c < 0x20 or c > 0x7E for c in s):
        raise Invalid("identifier not printable ASCII")
    return s.decode()


def check_uimage(img):
    if len(img) < 64:
        raise Invalid("uImage too short")
    magic, hcrc, _time, size, _load, _ep, dcrc, os_, arch, typ = struct.unpack(
        ">7I3B", img[:31])
    if magic != 0x27051956:
        raise Invalid("not a uImage")
    header = bytearray(img[:64])
    header[4:8] = bytes(4)
    if zlib.crc32(header) != hcrc:
        raise Invalid("uImage header CRC")
    if size != len(img) - 64:
        raise Invalid("uImage size")
    if zlib.crc32(img[64:]) != dcrc:
        raise Invalid("uImage data CRC")
    if (os_, arch, typ) != (5, 5, 2):
        raise Invalid("not a Linux MIPS kernel")


def check_squashfs(img):
    if len(img) < 96 or img[:4] != b"hsqs":
        raise Invalid("not SquashFS")
    block_size, = struct.unpack_from("<I", img, 12)
    block_log, = struct.unpack_from("<H", img, 22)
    major, minor = struct.unpack_from("<HH", img, 28)
    used, = struct.unpack_from("<Q", img, 40)
    if (major, minor) != (4, 0) or not 12 <= block_log <= 20 or block_size != 1 << block_log:
        raise Invalid("SquashFS superblock")
    if not 96 <= used <= len(img) or len(img) - used >= 4096 or any(img[used:]):
        raise Invalid("SquashFS size/padding")


def parse(buf):
    if len(buf) < MANIFEST_SIZE or buf[:8] != MAGIC:
        raise Invalid("not a bundle")
    version, header_size, flags = struct.unpack_from("<HHI", buf, 8)
    min_boot, count, record_size, length = struct.unpack_from("<IHHI", buf, 0x90)
    if (version, header_size, flags, count, record_size) != (1, HEADER_SIZE, 0, 2, RECORD_SIZE):
        raise Invalid("header fields")
    if any(buf[0x9C:DIGEST_AT]) or any(buf[DIGEST_AT + 32:0x100]):
        raise Invalid("reserved header bytes")
    if length != len(buf):
        raise Invalid("length")
    if manifest_digest(buf) != buf[DIGEST_AT:DIGEST_AT + 32]:
        raise Invalid("manifest digest")
    info = {
        "board": text_field(buf[0x10:0x30]),
        "layout": text_field(buf[0x30:0x50]),
        "build": text_field(buf[0x50:0x90]),
        "min_boot_compat": min_boot,
        "length": length,
    }
    end = MANIFEST_SIZE
    for i, (want, capacity, name) in enumerate(
            [(1, KERNEL_CAPACITY, "kernel"), (2, ROOTFS_CAPACITY, "rootfs")]):
        rec = buf[0x100 + i * RECORD_SIZE:0x100 + (i + 1) * RECORD_SIZE]
        typ, rflags, off, size, installed = struct.unpack_from("<HHIII", rec, 0)
        digest = rec[16:48]
        if typ != want or rflags or installed != size or any(rec[48:]):
            raise Invalid(name + " record")
        if size == 0 or off % ALIGN or off < end or off + size > len(buf) or any(buf[end:off]):
            raise Invalid(name + " placement")
        if size > capacity:
            raise Invalid(name + " too large")
        if hashlib.sha256(buf[off:off + size]).digest() != digest:
            raise Invalid(name + " digest")
        (check_uimage if i == 0 else check_squashfs)(buf[off:off + size])
        info[name] = "%d+%d sha256:%s" % (off, size, digest.hex())
        end = off + size
    if end != len(buf):
        raise Invalid("trailing bytes")
    return info


def build(board, layout, build_id, min_boot, kernel, rootfs):
    kernel_off = MANIFEST_SIZE
    rootfs_off = (kernel_off + len(kernel) + ALIGN - 1) // ALIGN * ALIGN
    total = rootfs_off + len(rootfs)
    out = bytearray(total)
    out[0:8] = MAGIC
    struct.pack_into("<HHI", out, 8, 1, HEADER_SIZE, 0)
    for at, size, text in ((0x10, 32, board), (0x30, 32, layout), (0x50, 64, build_id)):
        raw = text.encode()
        assert len(raw) < size
        out[at:at + len(raw)] = raw
    struct.pack_into("<IHHI", out, 0x90, min_boot, 2, RECORD_SIZE, total)
    for i, (typ, off, data) in enumerate(((1, kernel_off, kernel), (2, rootfs_off, rootfs))):
        rec = 0x100 + i * RECORD_SIZE
        struct.pack_into("<HHIII", out, rec, typ, 0, off, len(data), len(data))
        out[rec + 16:rec + 48] = hashlib.sha256(data).digest()
        out[off:off + len(data)] = data
    out[DIGEST_AT:DIGEST_AT + 32] = manifest_digest(out)
    return bytes(out)


def test_uimage(data_len):
    """fw_test.c make_uimage()"""
    data = bytes((i * 7 + 3) & 0xFF for i in range(data_len))
    header = bytearray(64)
    struct.pack_into(">I", header, 0, 0x27051956)
    struct.pack_into(">IIII", header, 12, data_len, 0x80010000, 0x80010000, zlib.crc32(data))
    header[28:32] = bytes((5, 5, 2, 3))
    header[32:45] = b"Linux-3.10.14"
    struct.pack_into(">I", header, 4, zlib.crc32(header))
    return bytes(header) + data


def test_squashfs(used):
    """fw_test.c make_squashfs()"""
    padded = (used + 4095) // 4096 * 4096
    img = bytearray(padded)
    for i in range(96, used):
        img[i] = (i * 13 + 1) & 0xFF
    img[0:4] = b"hsqs"
    img[14] = 0x04
    img[22] = 18
    img[28] = 4
    struct.pack_into("<I", img, 40, used)
    return bytes(img)


def main(argv):
    if len(argv) == 2 and argv[1] == "--golden":
        b = build("k1cam-t31l-gc2083", "k1cam-ab-8m-v1", "k1cam-test-1", 1,
                  test_uimage(5000), test_squashfs(9000))
        parse(b)
        print(hashlib.sha256(b).hexdigest())
        return 0
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1], "rb") as f:
        buf = f.read()
    try:
        info = parse(buf)
    except Invalid as e:
        print("%s: invalid: %s" % (argv[1], e), file=sys.stderr)
        return 1
    for key, value in info.items():
        print("%s=%s" % (key, value))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
