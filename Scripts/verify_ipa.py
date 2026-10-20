#!/usr/bin/env python3
"""Verifies that an .ipa can be installed on a device.

`package_ipa.sh` already inspects the app bundle before it zips it, but the
archive is the thing that gets shipped: a wrong layout, a duplicated bundle,
a simulator slice or a bundle identifier that does not match the plist inside
the zip all produce an .ipa that looks fine until iOS refuses it. This check
reads the archive with the standard library only, so it runs on Linux too.

Usage:
    python3 Scripts/verify_ipa.py build/artifacts/IpaStore-0.1.0.ipa
    python3 Scripts/verify_ipa.py --expect-bundle-id com.example.ipastore app.ipa
    python3 Scripts/verify_ipa.py --self-test
"""

from __future__ import annotations

import argparse
import io
import plistlib
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

CPU_ARCH_ABI64 = 0x01000000
CPU_TYPE_X86 = 7
CPU_TYPE_ARM = 12
CPU_TYPE_ARM64 = CPU_TYPE_ARM | CPU_ARCH_ABI64
CPU_TYPE_X86_64 = CPU_TYPE_X86 | CPU_ARCH_ABI64
MH_MAGIC_64 = 0xFEEDFACF
MH_MAGIC = 0xFEEDFACE
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF

ARCH_NAMES = {
    CPU_TYPE_ARM64: "arm64",
    CPU_TYPE_X86_64: "x86_64",
    CPU_TYPE_ARM: "arm",
    CPU_TYPE_X86: "i386",
}


class Invalid(Exception):
    """The archive cannot be installed as an .ipa."""


def macho_architectures(data: bytes) -> list[str]:
    """Returns the CPU names in a thin or fat Mach-O image."""
    if len(data) < 8:
        raise Invalid("the executable is too small to be Mach-O")
    little = struct.unpack("<I", data[:4])[0]
    big = struct.unpack(">I", data[:4])[0]

    # A thin image: the magic is stored in the image's own byte order.
    if little in (MH_MAGIC_64, MH_MAGIC):
        cputype = struct.unpack("<i", data[4:8])[0] & 0xFFFFFFFF
        return [ARCH_NAMES.get(cputype, hex(cputype))]
    if big in (MH_MAGIC_64, MH_MAGIC):
        cputype = struct.unpack(">i", data[4:8])[0] & 0xFFFFFFFF
        return [ARCH_NAMES.get(cputype, hex(cputype))]

    # A universal image: the fat header is big-endian, and its byte-swapped
    # form (FAT_CIGAM) appears when the whole file was written little-endian.
    if big in (FAT_MAGIC, FAT_MAGIC_64):
        order, magic = ">", big
    elif little in (FAT_MAGIC, FAT_MAGIC_64):
        order, magic = "<", little
    else:
        raise Invalid("the executable is not a Mach-O image")

    count = struct.unpack(order + "I", data[4:8])[0]
    if count == 0 or count > 64:
        raise Invalid(f"implausible fat header count {count}")
    entry_size = 32 if magic == FAT_MAGIC_64 else 20
    offset = 8
    found: list[str] = []
    for _ in range(count):
        if offset + entry_size > len(data):
            raise Invalid("the fat header is truncated")
        cputype = struct.unpack(order + "i", data[offset:offset + 4])[0]
        cputype &= 0xFFFFFFFF
        found.append(ARCH_NAMES.get(cputype, hex(cputype)))
        offset += entry_size
    return found


def verify(path: Path, expect_bundle_id: str | None = None,
           verbose: bool = True) -> dict[str, str]:
    def say(message: str) -> None:
        if verbose:
            print(message)

    if not path.exists():
        raise Invalid(f"{path} does not exist")
    try:
        archive = zipfile.ZipFile(path)
    except zipfile.BadZipFile as error:
        raise Invalid(f"the archive is not a zip file: {error}") from error

    with archive:
        names = archive.namelist()
        if not names:
            raise Invalid("the archive is empty")

        # --- Layout: Payload/<App>.app/... plus the optional metadata. ----
        # iTunes writes iTunesMetadata.plist next to Payload, and Xcode adds
        # SwiftSupport/ for Swift apps; anything else is unexpected.
        allowed = ("Payload/", "SwiftSupport/")
        allowed_files = ("iTunesMetadata.plist",)
        for name in names:
            if name.startswith("/") or ".." in Path(name).parts:
                raise Invalid(f"unsafe path in the archive: {name}")
            if name in allowed_files:
                continue
            if not name.startswith(allowed):
                raise Invalid(f"unexpected entry outside Payload/: {name}")

        apps = {
            name.split("/")[1]
            for name in names
            if name.startswith("Payload/") and len(name.split("/")) > 1
            and name.split("/")[1]
        }
        if len(apps) != 1:
            raise Invalid(
                "expected exactly one .app inside Payload/, found "
                + (", ".join(sorted(apps)) if apps else "none"))
        app = apps.pop()
        if not app.endswith(".app"):
            raise Invalid(f"Payload/{app} is not an .app bundle")
        root = f"Payload/{app}"
        say(f"bundle: {root}")

        # --- Info.plist is readable and self-consistent. ------------------
        plist_name = f"{root}/Info.plist"
        if plist_name not in names:
            raise Invalid(f"{plist_name} is missing")
        try:
            info = plistlib.loads(archive.read(plist_name))
        except Exception as error:  # noqa: BLE001 - report any decoder error
            raise Invalid(f"{plist_name} does not parse: {error}") from error
        for key in ("CFBundleIdentifier", "CFBundleExecutable",
                    "CFBundleShortVersionString", "MinimumOSVersion"):
            if not info.get(key):
                raise Invalid(f"{plist_name} has no {key}")
        say(f"identifier: {info['CFBundleIdentifier']}")
        say(f"version: {info['CFBundleShortVersionString']} "
            f"({info.get('CFBundleVersion', '?')})")
        say(f"minimum os: {info['MinimumOSVersion']}")
        if expect_bundle_id and info["CFBundleIdentifier"] != expect_bundle_id:
            raise Invalid(
                f"the bundle identifier is {info['CFBundleIdentifier']}, "
                f"expected {expect_bundle_id}")

        # --- The executable exists and only carries device slices. --------
        executable_name = f"{root}/{info['CFBundleExecutable']}"
        if executable_name not in names:
            raise Invalid(f"{executable_name} is missing")
        architectures = macho_architectures(archive.read(executable_name))
        say(f"architectures: {' '.join(architectures)}")
        if "arm64" not in architectures:
            raise Invalid(
                "the executable has no arm64 slice: " + " ".join(architectures))
        if "x86_64" in architectures or "i386" in architectures:
            raise Invalid(
                "the executable contains a simulator slice: "
                + " ".join(architectures))

        platform = info.get("DTPlatformName")
        if platform and platform != "iphoneos":
            raise Invalid(f"the bundle targets {platform}, not iphoneos")

        say(f"entries: {len(names)}")
        return {
            "bundle": root,
            "identifier": info["CFBundleIdentifier"],
            "version": info["CFBundleShortVersionString"],
            "minimum_os": str(info["MinimumOSVersion"]),
            "architectures": " ".join(architectures),
        }


def macho_stub(cputype: int) -> bytes:
    """A minimal 64-bit Mach-O header, enough to read the CPU type."""
    return struct.pack("<IiiII", MH_MAGIC_64, cputype, 0, 2, 0) + b"\0" * 24


def make_archive(path: Path, *, bundle_id: str = "com.example.ipastore",
                 cputype: int = CPU_TYPE_ARM64,
                 plist_platform: str = "iphoneos",
                 extra_entry: str | None = None) -> None:
    info = {
        "CFBundleIdentifier": bundle_id,
        "CFBundleExecutable": "IpaStore",
        "CFBundleShortVersionString": "0.1.0",
        "CFBundleVersion": "1",
        "MinimumOSVersion": "27.0",
        "DTPlatformName": plist_platform,
    }
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("Payload/IpaStore.app/Info.plist",
                         plistlib.dumps(info))
        archive.writestr("Payload/IpaStore.app/IpaStore", macho_stub(cputype))
        if extra_entry:
            archive.writestr(extra_entry, b"x")


def self_test() -> int:
    """Exercises both the accepted and the rejected layouts."""
    failures = 0
    cases: list[tuple[str, dict, str | None, bool]] = [
        ("arm64 device build", {}, None, True),
        ("wrong bundle id", {"bundle_id": "com.example.other"},
         "com.example.ipastore", False),
        ("simulator slice", {"cputype": CPU_TYPE_X86_64}, None, False),
        ("no arm64 slice", {"cputype": CPU_TYPE_ARM}, None, False),
        ("simulator platform", {"plist_platform": "iphonesimulator"}, None,
         False),
        ("iTunes metadata", {"extra_entry": "iTunesMetadata.plist"}, None,
         True),
        ("unknown entry", {"extra_entry": "evil.sh"}, None, False),
        ("zip slip", {"extra_entry": "Payload/../../etc/passwd"}, None, False),
    ]
    with tempfile.TemporaryDirectory() as directory:
        for name, options, expected_id, accepted in cases:
            path = Path(directory) / "case.ipa"
            make_archive(path, **options)
            try:
                verify(path, expect_bundle_id=expected_id, verbose=False)
                result = True
            except Invalid as error:
                result = False
                detail = str(error)
            if accepted and not result:
                failures += 1
                print(f"SELF TEST: FAIL {name}: rejected ({detail})")
            elif not accepted and result:
                failures += 1
                print(f"SELF TEST: FAIL {name}: accepted")
            else:
                print(f"SELF TEST: ok {name}")

        broken = Path(directory) / "broken.ipa"
        broken.write_bytes(b"not a zip")
        try:
            verify(broken, verbose=False)
            print("SELF TEST: FAIL not a zip: accepted")
            failures += 1
        except Invalid:
            print("SELF TEST: ok not a zip")

    print(f"SELF TEST: {'PASS' if failures == 0 else 'FAIL'} "
          f"({len(cases) + 1} cases)")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipa", nargs="?", type=Path)
    parser.add_argument("--expect-bundle-id", default=None)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    arguments = parser.parse_args()

    if arguments.self_test:
        return self_test()
    if arguments.ipa is None:
        parser.error("an .ipa path is required unless --self-test is used")

    try:
        summary = verify(arguments.ipa,
                         expect_bundle_id=arguments.expect_bundle_id,
                         verbose=not arguments.quiet)
    except Invalid as error:
        print(f"IPA verification: FAIL - {error}")
        return 1
    print("IPA verification: PASS "
          f"({summary['bundle']}, {summary['architectures']}, "
          f"{summary['minimum_os']}+)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
