#!/usr/bin/env python3
"""Stages the emulator's sensors HAL and RIL for the rootless engine.

    scripts/sensors/fetch-hal.py [--abi arm64-v8a|x86_64]... [--out app/src/main/jniLibs]

From the Android 13 emulator system image (google_apis, r17) it takes,
into <out>/<abi>/:
  libvine_sensors_multihal.so  android.hardware.sensors@2.1-service.multihal
  libvine_sensors_ranchu.so    android.hardware.sensors@2.1-impl.ranchu.so
  libvine_sensors_wakelock.so  android.hardware.sensors@2.0-ScopedWakelock.so
  libvine_rild.so, libvine_ril_*.so  libgoldfish-rild, libcuttlefish-ril-2.so
                               and the libraries it needs that Waydroid lacks
rootless_prep installs each set into the guest's /vendor when it is all there.

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
    # Keep in step with kRadioFiles in rootless_prep.cpp
    "libvine_rild.so": "/bin/hw/libgoldfish-rild",
    "libvine_ril_cuttlefish.so": "/lib64/libcuttlefish-ril-2.so",
    "libvine_ril_modem.so": "/lib64/libril-modem-lib.so",
    "libvine_ril_compat.so": "/lib64/hw/android.hardware.radio-library.compat.so",
    "libvine_ril_cf_fs.so": "/lib64/libcuttlefish_fs.so",
    "libvine_ril_cf_utils.so": "/lib64/libcuttlefish_utils.so",
    "libvine_ril_radio_1_2.so": "/lib64/android.hardware.radio@1.2.so",
    "libvine_ril_radio_1_3.so": "/lib64/android.hardware.radio@1.3.so",
    "libvine_ril_radio_1_4.so": "/lib64/android.hardware.radio@1.4.so",
    "libvine_ril_radio_1_5.so": "/lib64/android.hardware.radio@1.5.so",
    "libvine_ril_radio_1_6.so": "/lib64/android.hardware.radio@1.6.so",
    "libvine_ril_config_1_0.so": "/lib64/android.hardware.radio.config@1.0.so",
    "libvine_ril_config_1_1.so": "/lib64/android.hardware.radio.config@1.1.so",
    "libvine_ril_config_1_2.so": "/lib64/android.hardware.radio.config@1.2.so",
    "libvine_ril_config_1_3.so": "/lib64/android.hardware.radio.config@1.3.so",
    "libvine_ril_deprecated.so": "/lib64/android.hardware.radio.deprecated@1.0.so",
}
SHA256 = {
    "arm64-v8a": {
        "libvine_sensors_multihal.so": "1bd233fe6eb76a161c5f3a08ead60f10053ff3ab79f9b26b2777ded7ddf6ad15",
        "libvine_sensors_ranchu.so": "ea95b8153ab8653fa615faeef3f0bc9b8d520b3abc005299131cb22e81ee2e76",
        "libvine_sensors_wakelock.so": "0fc804fff97bea689c25f1428cbc50a82573dfe286e395ed8365b47d725a7caf",
        "libvine_rild.so": "5702fe3a58ce750bd9bb32232e91d2ad04a29eb0fdee5b2da171dfe4f46f708f",
        "libvine_ril_cuttlefish.so": "13b78ef4f43501422a5df4e16a5515941d2cbff6ecb7574f095d3bb38c1f7d49",
        "libvine_ril_modem.so": "6bbecc8be66bdd47553eb243773ecac976880af0a1760242c943259dc16f1c33",
        "libvine_ril_compat.so": "6070e2d575095aa1b4a1f04f77f27c4dd6f9a197be85378d4b346dbcc9c5b3e5",
        "libvine_ril_cf_fs.so": "cd67f3b49534d6fde715177aaa8b9392d6ed6cc31b7b7508b5656574314ea6c2",
        "libvine_ril_cf_utils.so": "24c8ca150e83f32ffd43fa09d6264aec2c19d3f0f2e1c472cb05238dc99a4bdc",
        "libvine_ril_radio_1_2.so": "05c63c4e0a318af92e4fa8a09070961fbc839b043945e2c2bd86884ea1ad7a9b",
        "libvine_ril_radio_1_3.so": "be53858923499561e140ed3efbef77699823b52122e0e7669260bcd098fbeeee",
        "libvine_ril_radio_1_4.so": "23d137015a9c690e516a6872403884bf1c26f509985dfd89576ce57bb7c1ff33",
        "libvine_ril_radio_1_5.so": "862a1f549f91ac4d08b8b22219c159e6938b085d2f6b600e1e9fc5cd2cd9f31e",
        "libvine_ril_radio_1_6.so": "2f5f7fcccd70b9eb13db9e006d778aed25defe8a9309c62153eabde4edb58a56",
        "libvine_ril_config_1_0.so": "8ff8a6d6dc341111d900395c37976290d340b2dcd8ed52067e8baa2b9e793b02",
        "libvine_ril_config_1_1.so": "e6c7f8027c5f85593c24eb64f246f5ef4f0964e7ca413669ea20eaf4553d3008",
        "libvine_ril_config_1_2.so": "5d79f1ca212d1d08ed1d5cf85cda00737a626f2cdd6c46ea99ab4972243e61bc",
        "libvine_ril_config_1_3.so": "3f3c0560432f1cdf04e8777d98d52b63791fd68c9512d93eee64bc98664c3608",
        "libvine_ril_deprecated.so": "43ed2ec1c9e5e916981e06920bb8ce143c0797268eaf49e0843f572a9b854b0a",
    },
    "x86_64": {
        "libvine_sensors_multihal.so": "24a9e818868b839e4594f02713584445ade95f15f333ab1a426d8078fa03ebee",
        "libvine_sensors_ranchu.so": "23a980c6e6cfa7afbbd3fb6f2b22c9dd87d2311177f11f61726e75b80ea5e3c0",
        "libvine_sensors_wakelock.so": "7178ef741b22d00240157af36041385ee85857c644a0b15f0ec90258f936a148",
        "libvine_rild.so": "e099b078bbef5b6e1d1ea6017b91fa84394faf7761e1b78782ddf9a3e4e391c4",
        "libvine_ril_cuttlefish.so": "84f14a3137949ade9c68ac210deee76cb2b7a8f8096c66a6e690e75892dd92b6",
        "libvine_ril_modem.so": "bfcf186287ff5a3d38a326a13cc19797651b24efcf841cfe8a9a12b2b2abbe22",
        "libvine_ril_compat.so": "582e1f863f12b3b0f4fec631767b431a14425c4a8c1017cec9c929bbf693beb2",
        "libvine_ril_cf_fs.so": "0fb9d79ba82f6c523dd7f085dacf57e68ef4f645e86f98d4cea43399a5008e57",
        "libvine_ril_cf_utils.so": "4a15adaaab43fb2e50ffbc6a02069ad196b4aad780880408d4379a69c093936b",
        "libvine_ril_radio_1_2.so": "d7fa590440b07102f6e5b05150619438ce459192803155f022ec6211c393ec47",
        "libvine_ril_radio_1_3.so": "f88ac49070012c2f4bf8d0ffea191002351da6f1ccae46f25c10c1caecc17f27",
        "libvine_ril_radio_1_4.so": "c14727f2c89b5c48bd2eb9fa3efe49f74e0aea5f708aa9ed8a5c720cd6574610",
        "libvine_ril_radio_1_5.so": "58f110479e99092f219bcb3c84e33abfc208e9bf6f9c7b7533ac60001845d827",
        "libvine_ril_radio_1_6.so": "3d09fac1f9936af1abccf9424428b0f09682d0b560909a048a46c217cb33b95a",
        "libvine_ril_config_1_0.so": "17b9b976fc3b7b277aa5cdc0d6172c72377ccbd9b742a3c72f9c4a40dcef0d4e",
        "libvine_ril_config_1_1.so": "cf3cd38e3aa7d7856983d67e4b3744f31ecdaafb78c7e5c54e41c5d739022041",
        "libvine_ril_config_1_2.so": "ee9a1d29d85daca7d95805eb45ab4d7a8dbbeda5755687842c3b1f572461d3d1",
        "libvine_ril_config_1_3.so": "044c9b838c90dcc658fc75e057a903dbeb040e9fb5d05f4e34bec253c27fd7aa",
        "libvine_ril_deprecated.so": "c4a60a196a9f9883328facbffcf6f6186a2d722379cbf876bee3c00dbe270506",
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
