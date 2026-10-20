"""Builds (and optionally installs and starts) the VHMD Link APK without Gradle:
NDK + CMake for the native library, aapt2 / zipalign / apksigner from the
Android SDK build-tools for the package.

    python headset/android/build_apk.py              # build
    python headset/android/build_apk.py --install    # build, adb install, start, adb reverse

Needs: Android SDK (ANDROID_HOME, or %LOCALAPPDATA%/Android/Sdk) with an NDK,
build-tools and a platform; CMake and Ninja on PATH; the Khronos OpenXR loader
AAR in headset/.cache (fetched with --fetch-loader, from Maven Central).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE = HERE.parent / ".cache"
BUILD = HERE / "build"
PACKAGE = "com.vhmd.link"
LOADER_VERSION = "1.1.63"
LOADER_AAR = f"openxr_loader_for_android-{LOADER_VERSION}.aar"
LOADER_SHA1 = "bb72fea1e7bdd35b381ae2a705206d912a1d5b30"
LOADER_URL = ("https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/"
              f"{LOADER_VERSION}/{LOADER_AAR}")
MIN_SDK, TARGET_SDK = 29, 32
PC_PORT = 39580


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def sdk_dir() -> Path:
    for v in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(v):
            return Path(os.environ[v])
    return Path(os.environ.get("LOCALAPPDATA", Path.home())) / "Android" / "Sdk"


def newest(d: Path) -> Path:
    items = sorted((p for p in d.iterdir() if p.is_dir()), key=lambda p: [int(x) if x.isdigit() else x
                                                                            for x in p.name.replace("-", ".").split(".")])
    if not items:
        raise SystemExit(f"nothing in {d}")
    return items[-1]


def tool(build_tools: Path, name: str) -> Path:
    for ext in (".exe", ".bat", ""):
        p = build_tools / (name + ext)
        if p.exists():
            return p
    raise SystemExit(f"{name} not found in {build_tools}")


def loader(fetch: bool) -> Path:
    """Unpacks the loader AAR's headers and arm64 library into build/openxr."""
    aar = CACHE / LOADER_AAR
    if not aar.exists():
        if not fetch:
            raise SystemExit(f"missing {aar}; run with --fetch-loader to download it from Maven Central")
        CACHE.mkdir(parents=True, exist_ok=True)
        print("downloading", LOADER_URL)
        urllib.request.urlretrieve(LOADER_URL, aar)
    if hashlib.sha1(aar.read_bytes()).hexdigest() != LOADER_SHA1:
        raise SystemExit(f"{aar} does not match the expected SHA-1")
    out = BUILD / "openxr"
    if not (out / "libopenxr_loader.so").exists():
        with zipfile.ZipFile(aar) as z:
            for name in z.namelist():
                if name.startswith("prefab/modules/headers/include/") and name.endswith(".h"):
                    dst = out / "include" / name.split("include/", 1)[1]
                elif name == "prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so":
                    dst = out / "libopenxr_loader.so"
                else:
                    continue
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(z.read(name))
    return out


def build(args) -> Path:
    sdk = sdk_dir()
    ndk = newest(sdk / "ndk")
    build_tools = newest(sdk / "build-tools")
    platforms = [p for p in (sdk / "platforms").iterdir() if (p / "android.jar").exists()]
    android_jar = max(platforms, key=lambda p: [int(x) for x in p.name.split("-")[1].split(".") if x.isdigit()]) / "android.jar"
    print(f"SDK {sdk}\nNDK {ndk.name}, build-tools {build_tools.name}, {android_jar.parent.name}")
    oxr = loader(args.fetch_loader)

    native = BUILD / "native"
    run(["cmake", "-S", HERE, "-B", native, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build/cmake/android.toolchain.cmake'}", "-DANDROID_ABI=arm64-v8a",
         f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DANDROID_STL=c++_static", f"-DOPENXR_DIR={oxr.as_posix()}"])
    run(["cmake", "--build", native])

    base = BUILD / "base.apk"
    run([tool(build_tools, "aapt2"), "link", "-o", base, "--manifest", HERE / "AndroidManifest.xml",
         "-I", android_jar, "--min-sdk-version", MIN_SDK, "--target-sdk-version", TARGET_SDK,
         "--version-code", "1", "--version-name", "0.1.0", "--debug-mode"])
    unaligned = BUILD / "unaligned.apk"
    shutil.copy(base, unaligned)
    with zipfile.ZipFile(unaligned, "a") as z:
        # Stored, not compressed: with extractNativeLibs=false they are mapped in place.
        z.write(native / "libvhmdlink.so", "lib/arm64-v8a/libvhmdlink.so", compress_type=zipfile.ZIP_STORED)
        z.write(oxr / "libopenxr_loader.so", "lib/arm64-v8a/libopenxr_loader.so", compress_type=zipfile.ZIP_STORED)
    aligned = BUILD / "aligned.apk"
    run([tool(build_tools, "zipalign"), "-f", "-P", "16", "4", unaligned, aligned])

    # Signed with the local Android debug key (created by Android Studio / the SDK).
    keystore = Path.home() / ".android" / "debug.keystore"
    if not keystore.exists():
        raise SystemExit(f"no debug keystore at {keystore}")
    apk = BUILD / "vhmd-link.apk"
    run([tool(build_tools, "apksigner"), "sign", "--ks", keystore, "--ks-key-alias", "androiddebugkey",
         "--ks-pass", "pass:android", "--key-pass", "pass:android", "--out", apk, aligned])
    print("built", apk, f"{apk.stat().st_size / 1024:.0f} KiB")
    return apk


def install(apk: Path) -> None:
    run(["adb", "install", "-r", "-g", apk])
    run(["adb", "reverse", f"tcp:{PC_PORT}", f"tcp:{PC_PORT}"])
    run(["adb", "shell", "am", "start", "-n", f"{PACKAGE}/android.app.NativeActivity"])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", action="store_true", help="adb install, adb reverse and start the app")
    ap.add_argument("--fetch-loader", action="store_true", help=f"download {LOADER_AAR} if missing")
    args = ap.parse_args()
    apk = build(args)
    if args.install:
        install(apk)


if __name__ == "__main__":
    main()
