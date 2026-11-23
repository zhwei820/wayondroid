#!/usr/bin/env python3
"""Stages the emulator's sensors HAL for the rootless engine.

    scripts/sensors/fetch-hal.py [--abi arm64-v8a|x86_64]... [--out app/src/main/jniLibs]

From the Android 13 emulator system image (google_apis, r17) it takes,
into <out>/<abi>/:
  libvine_sensors_multihal.so  android.hardware.sensors@2.1-service.multihal
  libvine_sensors_ranchu.so    android.hardware.sensors@2.1-impl.ranchu.so
  libvine_sensors_wakelock.so  android.hardware.sensors@2.0-ScopedWakelock.so
rootless_prep installs them into the guest's /vendor when all are there.

Only vendor.img is downloaded (HTTP ranges into the 1.7 GB zip); its
GPT holds one ext4 partition, read with debugfs (e2fsprogs). The zip
entry's CRC and the pinned hashes below check what comes out.

These are binaries from an Android SDK system image, under the Android
SDK License, not AOSP sources.
"""
import argparse
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

BASE = "https://dl.google.com/android/repository/sys-img/google_apis/"
IMAGES = {"arm64-v8a": "arm64-v8a-33_r17.zip", "x86_64": "x86_64-33_r17.zip"}
FILES = {
    "libvine_sensors_multihal.so": "/bin/hw/android.hardware.sensors@2.1-service.multihal",
    "libvine_sensors_ranchu.so": "/lib64/hw/android.hardware.sensors@2.1-impl.ranchu.so",
    "libvine_sensors_wakelock.so": "/lib64/android.hardware.sensors@2.0-ScopedWakelock.so",
}
SHA256 = {
    "arm64-v8a": {
        "libvine_sensors_multihal.so": "1bd233fe6eb76a161c5f3a08ead60f10053ff3ab79f9b26b2777ded7ddf6ad15",
        "libvine_sensors_ranchu.so": "ea95b8153ab8653fa615faeef3f0bc9b8d520b3abc005299131cb22e81ee2e76",
        "libvine_sensors_wakelock.so": "0fc804fff97bea689c25f1428cbc50a82573dfe286e395ed8365b47d725a7caf",
    },
    "x86_64": {
        "libvine_sensors_multihal.so": "24a9e818868b839e4594f02713584445ade95f15f333ab1a426d8078fa03ebee",
        "libvine_sensors_ranchu.so": "23a980c6e6cfa7afbbd3fb6f2b22c9dd87d2311177f11f61726e75b80ea5e3c0",
        "libvine_sensors_wakelock.so": "7178ef741b22d00240157af36041385ee85857c644a0b15f0ec90258f936a148",
    },
}


class RangeFile(io.RawIOBase):
    """A remote file read through HTTP range requests"""

    def __init__(self, url):
        self.url, self.pos = url, 0
        with urllib.request.urlopen(urllib.request.Request(url, method="HEAD")) as r:
            self.size = int(r.headers["Content-Length"])

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def readinto(self, b):
        if self.pos >= self.size or not len(b):
            return 0
        end = min(self.pos + len(b), self.size) - 1
        req = urllib.request.Request(self.url, headers={"Range": f"bytes={self.pos}-{end}"})
        with urllib.request.urlopen(req) as r:
            data = r.read()
        b[: len(data)] = data
        self.pos += len(data)
        return len(data)


def vendor_partition(img, out):
    """Copies the GPT's first partition (vendor) out of img"""
    with open(img, "rb") as f:
        f.seek(512)
        hdr = f.read(92)
        if hdr[:8] != b"EFI PART":
            shutil.copyfile(img, out)  # already a bare filesystem
            return
        lba, = struct.unpack_from("<Q", hdr, 72)
        count, size = struct.unpack_from("<II", hdr, 80)
        f.seek(lba * 512)
        for _ in range(count):
            e = f.read(size)
            if e[:16] == bytes(16):
                continue
            first, last = struct.unpack_from("<QQ", e, 32)
            f.seek(first * 512)
            with open(out, "wb") as o:
                left = (last - first + 1) * 512
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    o.write(chunk)
                    left -= len(chunk)
            return
    sys.exit("vendor.img: no partition")


def fetch(abi, out_dir, work):
    url = BASE + IMAGES[abi]
    print(f"{abi}: vendor.img from {url}")
    zf = zipfile.ZipFile(io.BufferedReader(RangeFile(url), buffer_size=8 << 20))
    img = os.path.join(work, abi + "-vendor.img")
    with zf.open(f"{abi}/vendor.img") as src, open(img, "wb") as dst:  # checks the entry's CRC
        shutil.copyfileobj(src, dst, 8 << 20)
    fs = img + ".ext4"
    vendor_partition(img, fs)
    os.makedirs(out_dir, exist_ok=True)
    for name, path in FILES.items():
        tmp = os.path.join(work, name)
        subprocess.run(["debugfs", "-R", f"dump {path} {tmp}", fs], check=True, capture_output=True)
        if not os.path.isfile(tmp) or not os.path.getsize(tmp):
            sys.exit(f"{abi}: {path} is not in vendor.img")
        digest = hashlib.sha256(open(tmp, "rb").read()).hexdigest()
        want = SHA256[abi][name]
        if digest != want:
            sys.exit(f"{abi}: {name}: sha256 {digest}, expected {want}")
        shutil.copyfile(tmp, os.path.join(out_dir, name))
        os.chmod(os.path.join(out_dir, name), 0o755)
        print(f"  {name}")


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap = argparse.ArgumentParser()
    ap.add_argument("--abi", action="append", choices=sorted(IMAGES))
    ap.add_argument("--out", default=os.path.join(root, "app/src/main/jniLibs"))
    args = ap.parse_args()
    if not shutil.which("debugfs"):
        sys.exit("debugfs (e2fsprogs) is required")
    with tempfile.TemporaryDirectory() as work:
        for abi in args.abi or sorted(IMAGES):
            fetch(abi, os.path.join(args.out, abi), work)


if __name__ == "__main__":
    main()
