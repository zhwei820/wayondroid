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
  libvine_cam_*.so             Google's camera HAL (emulated sensor), its
                               libraries Waydroid lacks and its configurations
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
    # Keep in step with kCameraFiles in rootless_prep.cpp: Google's camera HAL
    # with the emulated sensor HWL, which renders a test scene, and what it
    # needs that Waydroid lacks
    "libvine_cam_provider.so": "/bin/hw/android.hardware.camera.provider@2.7-service-google",
    "libvine_cam_hal.so": "/lib64/libgooglecamerahal.so",
    "libvine_cam_halutils.so": "/lib64/libgooglecamerahalutils.so",
    "libvine_cam_hwl.so": "/lib64/libgooglecamerahwl_impl.so",
    "libvine_cam_profiler.so": "/lib64/lib_profiler.so",
    "libvine_cam_meminfo.so": "/lib64/libmeminfo.so",
    "libvine_cam_grallocusage.so": "/lib64/libgrallocusage.so",
    "libvine_cam_thermal_2_0.so": "/lib64/android.hardware.thermal@2.0.so",
    "libvine_cam_thermal_1_0.so": "/lib64/android.hardware.thermal@1.0.so",
    # The HWL's camera configurations (JSON, under a library's name)
    "libvine_cam_back.so": "/etc/config/emu_camera_back.json",
    "libvine_cam_front.so": "/etc/config/emu_camera_front.json",
}


def _load(data):
    import json
    import re
    text = re.sub(r",(\s*[\]}])", r"\1", data.decode())  # jsoncpp takes trailing commas, json does not
    return json.loads(text)


# A Pixel 5's: sensor (pixels, physical mm = pixels x pitch), lens
PIXEL5_CAMERAS = {
    # IMX363, 1.4 um, 4.38 mm f/1.73
    "back": dict(size=(4032, 3024), physical=("5.64480019", "4.23360014"), focal="4.38000011",
                 aperture="1.73000002", facing="BACK", orientation="90", level="3"),
    # IMX355, 1.12 um, f/2.0; the focal length from its 84 degree diagonal field of view
    "front": dict(size=(3264, 2448), physical=("3.65568018", "2.74176002"), focal="2.50999999",
                  aperture="2.00000000", facing="FRONT", orientation="270", level="FULL"),
}
SIZED_KEYS = ("android.scaler.availableStreamConfigurations", "android.scaler.availableMinFrameDurations",
              "android.scaler.availableStallDurations", "android.control.availableExtendedSceneModeMaxSizes")


def pixel5_camera(back_data, which):
    """The emulator's back camera (one sensor, LEVEL_3, RAW) as a Pixel 5
    camera: its largest size becomes the Pixel 5 sensor's, wherever a size
    appears, and the optics are the Pixel 5's. The test scene stays."""
    import json
    cam = _load(back_data)
    spec = PIXEL5_CAMERAS[which]
    w0, h0 = (int(v) for v in cam["android.sensor.info.pixelArraySize"])
    w, h = spec["size"]
    for key in SIZED_KEYS:
        v = cam[key]
        for i in range(len(v) - 1):
            if v[i] == str(w0) and v[i + 1] == str(h0):
                v[i], v[i + 1] = str(w), str(h)
    cam["android.sensor.info.pixelArraySize"] = [str(w), str(h)]
    for key in ("android.sensor.info.activeArraySize", "android.sensor.info.preCorrectionActiveArraySize"):
        cam[key] = ["0", "0", str(w), str(h)]
    cam["android.jpeg.maxSize"] = [str(w * h)]
    cam["android.sensor.info.physicalSize"] = list(spec["physical"])
    cam["android.lens.info.availableFocalLengths"] = [spec["focal"]]
    cam["android.lens.info.availableApertures"] = [spec["aperture"]]
    cam["android.lens.facing"] = [spec["facing"]]
    cam["android.sensor.orientation"] = [spec["orientation"]]
    cam["android.info.supportedHardwareLevel"] = [spec["level"]]
    return json.dumps(cam, indent=1).encode()


_back = {}


def back_camera(data):
    _back["data"] = data
    return pixel5_camera(data, "back")


def front_camera(data):
    """The emulator's front configuration is a logical camera over three
    ultra-high-resolution physical ones; a Pixel 5 has one plain front
    sensor, so it is made from the back configuration (FILES lists it first)"""
    return pixel5_camera(_back["data"], "front")


# EmulatedCameraDeviceSessionHwlImpl::ConfigurePipeline gives an
# IMPLEMENTATION_DEFINED stream (a preview) YCBCR_420_888 buffers, which the
# emulator's host composes; the guest's SurfaceFlinger can't import them
# (gfxstream exports no AHardwareBuffer for them) and aborts. Here they are
# RGBA_8888, which EmulatedSensor fills with CaptureRGB. arm64: the
# `mov w8, #0x23` among its neighbours becomes `mov w8, #0x1`.
HWL_PREVIEW_FROM = bytes.fromhex("bf8a0071810200546804805205000014e803152a")
HWL_PREVIEW_TO = bytes.fromhex("bf8a0071810200542800805205000014e803152a")


def rgba_previews(data):
    if data.count(HWL_PREVIEW_FROM) != 1:
        return data
    return data.replace(HWL_PREVIEW_FROM, HWL_PREVIEW_TO)


# Applied after the hash check
TRANSFORMS = {"libvine_cam_back.so": back_camera, "libvine_cam_front.so": front_camera,
              "libvine_cam_hwl.so": rgba_previews}
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
        "libvine_cam_provider.so": "15633db83ca06df97bda3b554ca2d77f8745596cadcf821be55a40ed63feb5cb",
        "libvine_cam_hal.so": "fda18c90518b917005029c301a8106aea534a4603ec9b57d2561b0b0a80ba7e7",
        "libvine_cam_halutils.so": "d9d8ddd54247f8fba31c7137c3a7b3337aeec02d1fcabc88f17b886bede3a079",
        "libvine_cam_hwl.so": "8f14f04bc40be9abce6d793c3dce463b9931d97805eafc345ef16be1d24714e6",
        "libvine_cam_profiler.so": "065956067c5890f25466411a68b46b9028a1859d4c84de5d66657143be300362",
        "libvine_cam_meminfo.so": "ef55d0fd7d578561b77af7875e97c80665e9a491c213525881fe69edc87dc5d5",
        "libvine_cam_grallocusage.so": "2800b4228d5924d883c59feb261fc681c8cbd00c50896920707158f0ef151a78",
        "libvine_cam_thermal_2_0.so": "76a3fe059b949e0c4350c63b728c781e217beb44270a5bb2760a086038ab4169",
        "libvine_cam_thermal_1_0.so": "7875c2d6de96f2397b34c39cfb9edb5732006c4dc61397abba219c28025ed9c2",
        "libvine_cam_back.so": "8f6d68c50674023d13a155aaa5cadcce59df2e689e65eb8d509862ce71cfa479",
        "libvine_cam_front.so": "cafebf4cdc2188021e1b78c01ecba0d33088851d37ce53e80adcb200ce2ddff4",
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
        "libvine_cam_provider.so": "8dda5b74f09e11bbc537dd8bf447bef075b319fba8b4e3c53921364a3aed9d61",
        "libvine_cam_hal.so": "2f3fcfa9916385649a5198a84db074ebad28603b848b3c5359ca70d3ddf3807b",
        "libvine_cam_halutils.so": "a8e1c7f2b62d3441e7f62a51e0590f12fb540093f1e79df3a3dbae39e5468219",
        "libvine_cam_hwl.so": "33112ba066639428c94102d5662062328d54dca299bc411bf62d9a87fae76e51",
        "libvine_cam_profiler.so": "7d1119205a045d82cc3ff1cd67e5a694c50b982445e2af7a463d692f250074eb",
        "libvine_cam_meminfo.so": "8f72e20b8c3139a98ab2db9fad7ca0a9cbb9b607a69114c2bef33d16118e2873",
        "libvine_cam_grallocusage.so": "1f1e216a1251651bff216a22da373a60a2fff7b212264ccc1cd6f51ac842d1cf",
        "libvine_cam_thermal_2_0.so": "9e5487daabbfa8f505d68ef8eeea5e58d04225a6a7971487f736aa2028337f18",
        "libvine_cam_thermal_1_0.so": "31f3d928ae1a89c5355340a6b13426a9db1ada82559e47c43fea60527b439b01",
        "libvine_cam_back.so": "8f6d68c50674023d13a155aaa5cadcce59df2e689e65eb8d509862ce71cfa479",
        "libvine_cam_front.so": "cafebf4cdc2188021e1b78c01ecba0d33088851d37ce53e80adcb200ce2ddff4",
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
        if name in TRANSFORMS:
            with open(tmp, "rb") as f:
                data = TRANSFORMS[name](f.read())
            with open(tmp, "wb") as f:
                f.write(data)
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
