#!/usr/bin/env python3
"""Archive the current build as a revision in releases/<version>/.

    python tools/make_release.py --notes "what changed"
    python tools/make_release.py --name 0.8.3-noconfirm --dir test --notes "..."
    python tools/make_release.py --from-ssw build/smartswitch-0.8.0.ssw --notes "..."

Build F407_smartswitch (Debug) and, if it changed, Bootloader (Release) first. Writes:

    releases/<version>/smartswitch-<version>.ssw       web update (Firmware tab), header + SHA-256
    releases/<version>/smartswitch-<version>-app.hex   same image for ST-LINK (at 0x08008000)
    releases/<version>/bootloader-<blver>.hex          bootloader for ST-LINK (0x08000000)
    releases/<version>/RELEASE.txt                     version, build, git commit, SHA-256, notes

The images contain the web password (web_secrets.h) in plain text: releases/ is ignored by
git and must never be published.
"""
import argparse
import datetime
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP_ELF = os.path.join(ROOT, "F407_smartswitch", "Debug", "F407_smartswitch.elf")
BL_ELF = os.path.join(ROOT, "Bootloader", "Release", "Bootloader.elf")
FW_H = os.path.join(ROOT, "F407_smartswitch", "Core", "Inc", "fw_version.h")
BL_H = os.path.join(ROOT, "Bootloader", "Core", "Inc", "bl_main.h")

sys.path.insert(0, HERE)
import make_ssw  # noqa: E402


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def git(*args):
    try:
        return subprocess.check_output(["git", "-C", ROOT] + list(args), text=True).strip()
    except Exception:
        return "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--notes", default="", help="what changed in this revision")
    ap.add_argument("--name", help="folder/file name instead of the version (e.g. 0.8.3-noconfirm)")
    ap.add_argument("--dir", default="", help="sub-folder of releases/ (e.g. test)")
    ap.add_argument("--force", action="store_true", help="overwrite an existing folder")
    ap.add_argument("--from-ssw", help="archive an existing .ssw instead of the current build")
    a = ap.parse_args()

    ver = re.search(r'FW_VERSION_STR\s+"([^"]+)"', open(FW_H).read()).group(1)
    if a.from_ssw:
        with open(a.from_ssw, "rb") as f:
            v = int.from_bytes(f.read(28)[24:28], "little")
        ver = f"{v >> 16}.{(v >> 8) & 255}.{v & 255}"
    blver = re.search(r'BL_VERSION\s+"([^"]+)"', open(BL_H).read()).group(1)
    name = a.name or ver
    out = os.path.join(ROOT, "releases", a.dir, name)
    if os.path.exists(out) and not a.force:
        sys.exit(f"{out} exists (use --force or bump the version)")
    os.makedirs(out, exist_ok=True)

    objcopy = make_ssw.find_objcopy()
    ssw = os.path.join(out, f"smartswitch-{name}.ssw")
    if a.from_ssw:
        shutil.copyfile(a.from_ssw, ssw)
    else:
        subprocess.check_call([sys.executable, os.path.join(HERE, "make_ssw.py"), APP_ELF, "-o", ssw])
    app_hex = os.path.join(out, f"smartswitch-{name}-app.hex")
    subprocess.check_call([objcopy, "-I", "binary", "-O", "ihex", "--change-addresses", "0x08008000", ssw, app_hex])
    bl_hex = os.path.join(out, f"bootloader-{blver}.hex")
    subprocess.check_call([objcopy, "-O", "ihex", BL_ELF, bl_hex])

    with open(ssw, "rb") as f:
        build = f.read(60)[28:60].split(b"\0")[0].decode(errors="replace")
    dirty = git("status", "--porcelain", "--", "F407_smartswitch/Core", "Bootloader/Core")
    lines = [
        f"SmartSwitch firmware {name}",
        f"version      {ver} (build {build})",
        f"bootloader   {blver}",
        ("git commit   unknown (archived from an existing image)" if a.from_ssw else
         f"git commit   {git('rev-parse', '--short', 'HEAD')}{' + uncommitted changes' if dirty else ''}"),
        f"archived     {datetime.datetime.now():%Y-%m-%d %H:%M}",
        "",
        "Files",
        f"  {os.path.basename(ssw)}  web update: Firmware tab -> Upload -> Install",
        f"  {os.path.basename(app_hex)}  ST-LINK, application only (fit DIS_WDT)",
        f"  {os.path.basename(bl_hex)}  ST-LINK, bootloader (only for a new or erased board)",
        "",
        "SHA-256",
    ] + [f"  {sha(p)}  {os.path.basename(p)}" for p in (ssw, app_hex, bl_hex)] + [
        "",
        "Notes",
        "  " + (a.notes or "-"),
        "",
        "Contains the web password in plain text: keep private, never publish.",
    ]
    with open(os.path.join(out, "RELEASE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"archived {name} -> {os.path.relpath(out, ROOT)}")


if __name__ == "__main__":
    main()
