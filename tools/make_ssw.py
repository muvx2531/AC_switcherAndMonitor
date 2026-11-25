#!/usr/bin/env python3
"""Turn the application ELF/BIN into a release firmware image (.ssw) for web updates.

    python tools/make_ssw.py                      # Debug build -> build/smartswitch-<ver>.ssw
    python tools/make_ssw.py path/to/app.bin -o out.ssw

The application links a 512-byte header at 0x08008000 (Core/Src/app_image.c, format in
Core/Inc/ssw_image.h) marked DEV. This tool fills it in: magic "SSWF", payload size,
SHA-256 of the payload and the header CRC-32. The .ssw file is the whole image (header +
payload); it can also be programmed with ST-LINK at 0x08008000.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import zlib

APP_ADDR = 0x08008000
HEADER_SIZE = 512
MAGIC_RELEASE = 0x46575353
MAGIC_DEV = 0x44575353
BOARD = b"SSW-F407"

# offsets inside SswHeader (keep in sync with ssw_image.h)
OFF_MAGIC = 0
OFF_HVER = 4
OFF_HSIZE = 6
OFF_BOARD = 8
OFF_FWVER = 24
OFF_BUILD = 28
OFF_LOAD = 60
OFF_VOFF = 64
OFF_END = 68
OFF_PSIZE = 72
OFF_SHA = 76
OFF_CRC = 508

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def find_objcopy():
    plugins = r"C:\ST\STM32CubeIDE_1.16.0\STM32CubeIDE\plugins"
    if os.path.isdir(plugins):
        for d in sorted(os.listdir(plugins)):
            if "gnu-tools-for-stm32" in d:
                exe = os.path.join(plugins, d, "tools", "bin", "arm-none-eabi-objcopy.exe")
                if os.path.exists(exe):
                    return exe
    return "arm-none-eabi-objcopy"


def load_binary(path):
    if path.lower().endswith(".elf"):
        out = path[:-4] + ".bin"
        subprocess.check_call([find_objcopy(), "-O", "binary", path, out])
        path = out
    with open(path, "rb") as f:
        return bytearray(f.read())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", default=os.path.join(ROOT, "F407_smartswitch", "Debug", "F407_smartswitch.elf"))
    ap.add_argument("-o", "--output")
    a = ap.parse_args()

    img = load_binary(a.input)
    if len(img) < HEADER_SIZE + 8:
        sys.exit("image too small")
    magic, = struct.unpack_from("<I", img, OFF_MAGIC)
    if magic not in (MAGIC_DEV, MAGIC_RELEASE):
        sys.exit("no SmartSwitch header at the start of the image (is it linked at 0x08008000?)")
    if img[OFF_BOARD:OFF_BOARD + len(BOARD)] != BOARD:
        sys.exit("board id mismatch")
    load, voff, end = struct.unpack_from("<III", img, OFF_LOAD)
    if load != APP_ADDR or voff != 0x200:
        sys.exit("unexpected load address / vector offset")
    size = end - APP_ADDR
    if size > len(img):
        sys.exit("image_end beyond the binary (%d > %d)" % (size, len(img)))
    img = img[:size]
    payload = bytes(img[HEADER_SIZE:])
    struct.pack_into("<I", img, OFF_MAGIC, MAGIC_RELEASE)
    struct.pack_into("<I", img, OFF_PSIZE, len(payload))
    img[OFF_SHA:OFF_SHA + 32] = hashlib.sha256(payload).digest()
    struct.pack_into("<I", img, OFF_CRC, zlib.crc32(bytes(img[:OFF_CRC])) & 0xFFFFFFFF)

    fwver, = struct.unpack_from("<I", img, OFF_FWVER)
    ver = "%d.%d.%d" % (fwver >> 16, (fwver >> 8) & 255, fwver & 255)
    build = img[OFF_BUILD:OFF_BUILD + 32].split(b"\0")[0].decode("ascii", "replace")
    out = a.output or os.path.join(ROOT, "build", "smartswitch-%s.ssw" % ver)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(img)
    print("wrote %s: version %s (%s), %d bytes, sha256 %s" % (out, ver, build, len(img), hashlib.sha256(payload).hexdigest()[:16]))


if __name__ == "__main__":
    main()
