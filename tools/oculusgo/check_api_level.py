#!/usr/bin/env python3
"""Reports strong symbol references that an Android release cannot resolve.

Every shared object is checked against the NDK stub libraries for the given API level plus the
other libraries packaged beside it. Weak references are skipped: code built with
__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ checks for them at run time. --preload names a packaged
library the app puts in LD_PRELOAD for helper executables; --optional names libraries whose load
failure the app already handles (it then reports them without failing).
"""

import argparse
import os
import pathlib
import subprocess
import sys
import zipfile
import tempfile

ELF_MAGIC = b"\x7fELF"


def nm(tool, path, *flags):
    result = subprocess.run([tool, "-D", *flags, str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return result.stdout.splitlines()


def defined(tool, path):
    lines = nm(tool, path, "--defined-only") or []
    return {line.split()[-1].split("@")[0] for line in lines if len(line.split()) >= 3}


def strong_undefined(tool, path):
    lines = nm(tool, path, "--undefined-only")
    if lines is None:
        return None
    names = set()
    for line in lines:
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U":
            names.add(parts[-1].split("@")[0])
    return names


def needed(readelf, path):
    result = subprocess.run([readelf, "-d", str(path)], capture_output=True, text=True)
    libs = []
    for line in result.stdout.splitlines():
        if "(NEEDED)" in line and "[" in line:
            libs.append(line.split("[", 1)[1].rstrip("]"))
    return libs


def elf_files(root):
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.is_file() and not path.is_symlink():
            with open(path, "rb") as handle:
                if handle.read(4) == ELF_MAGIC:
                    yield path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", help="an APK, or a directory of arm64 shared objects")
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME", ""))
    parser.add_argument("--api", type=int, default=25)
    parser.add_argument("--preload", action="append", default=[])
    parser.add_argument("--optional", action="append", default=[])
    args = parser.parse_args()

    prebuilt = pathlib.Path(args.ndk) / "toolchains/llvm/prebuilt/linux-x86_64"
    tool = str(prebuilt / "bin/llvm-nm")
    readelf = str(prebuilt / "bin/llvm-readelf")
    sysroot = prebuilt / "sysroot/usr/lib/aarch64-linux-android"
    # The NDK ships no separate stubs for API 25; it is identical to 24 for native code.
    level = 24 if args.api == 25 else args.api
    stubs = sysroot / str(level)
    if not stubs.is_dir():
        sys.exit(f"no NDK stubs for API {level} under {sysroot}")

    with tempfile.TemporaryDirectory() as scratch:
        root = pathlib.Path(args.target)
        if root.is_file():
            with zipfile.ZipFile(root) as apk:
                for name in apk.namelist():
                    if name.startswith("lib/arm64-v8a/"):
                        apk.extract(name, scratch)
            root = pathlib.Path(scratch) / "lib/arm64-v8a"

        system = {}
        for stub in stubs.glob("*.so"):
            system[stub.name] = defined(tool, stub)
        system["libc++_shared.so"] = set()
        packaged = {path.name: defined(tool, path) for path in elf_files(root)}

        preloaded = set()
        for name in args.preload:
            if name not in packaged:
                sys.exit(f"--preload {name} is not packaged")
            preloaded |= packaged[name]
        failures = 0
        for path in elf_files(root):
            missing = strong_undefined(tool, path)
            if missing is None:
                continue
            available = set(preloaded)
            unknown = []
            for lib in needed(readelf, path):
                if lib in packaged:
                    available |= packaged[lib]
                elif lib in system:
                    available |= system[lib]
                else:
                    unknown.append(lib)
            if unknown:
                failures += 1
                print(f"{path.name}: needs libraries absent from API {args.api}: {', '.join(unknown)}")
                continue
            if any(lib == "libc++_shared.so" for lib in needed(readelf, path)) and "libc++_shared.so" not in packaged:
                continue
            gaps = sorted(missing - available)
            if gaps:
                optional = path.name in args.optional
                failures += 0 if optional else 1
                shown = ", ".join(gaps[:12]) + (" ..." if len(gaps) > 12 else "")
                note = " (optional: the app falls back)" if optional else ""
                print(f"{path.name}: {len(gaps)} symbols missing on API {args.api}{note}: {shown}")
        print(f"checked {len(packaged)} ELF files for API {args.api}: {failures} incompatible")
        return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
