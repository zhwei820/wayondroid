#!/usr/bin/env python3
"""Drives vine_helper --rootless like the app does, against a fake Waydroid
image (mke2fs -d) and stand-ins for tawcroot, binderd, vine_wl and
vine_pulse. The base dir is deep enough that socket paths under it do not
fit in sun_path, like an app data dir.

Usage: test_rootless.py VINE_HELPER"""
import os
import queue
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

helper_bin = sys.argv[1]
failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("  FAIL", msg)


def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def unesc(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append({"n": "\n", "r": "\r", "t": "\t"}.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


# tawcroot: --init runs the fake init, anything else runs the command on the host
FAKE_TAWCROOT = r"""#!/bin/sh
here=${0%/*}
init=0; run=
while [ $# -gt 0 ]; do
  case $1 in
    --) shift; break ;;
    --init) init=1 ;;
    -r) shift ;;
    -b) shift; case $1 in *:/run/user/1000) run=${1%:/run/user/1000} ;; esac ;;
  esac
  shift
done
guest_path=$PATH
PATH=$PATH:/usr/bin:/bin
if [ $init = 1 ]; then
  echo "init PATH=$guest_path" >> "$run/../init-env.txt"
  # A daemon that outlives its parent, like the zygotes
  (sh -c 'echo $$ > "$1/../orphan.pid"; exec sleep 1000' sh "$run" &)
  if [ -e "$run/../init-crash" ]; then sleep 0.3; exit 3; fi
  sleep 0.5
  echo 1 > "$run/boot_completed"
  exec sleep 1000
fi
[ "$1" = /system/bin/sh ] && shift && set -- sh "$@"
PATH=$here/fakebin:$PATH exec "$@"
"""

FAKE_BINDERD = """#!/bin/sh
mkdir -p "$1" && : > "$1/ready" && exec sleep 1000
"""

FAKE_PULSE = """#!/bin/sh
exec sleep 1000
"""

# vine_wl: makes the vinefb file, records input lines next to it
FAKE_WL = r"""#!/usr/bin/env python3
import os, socket, sys
a = sys.argv[1:]
opt = {a[i]: a[i + 1] for i in range(0, len(a) - 1, 2)}
open(opt["--vinefb"], "wb").write(b"\0" * 4096)
open("wl-args.txt", "w").write(" ".join(a) + "\n")
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
if os.path.exists(opt["--input"]):
    os.unlink(opt["--input"])
s.bind(opt["--input"])
s.listen(4)
log = open("wl-lines.txt", "a", buffering=1)
while True:
    c, _ = s.accept()
    buf = b""
    while True:
        d = c.recv(4096)
        if not d:
            break
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            log.write(line.decode() + "\n")
"""

FAKE_CMD = r"""#!/bin/sh
echo "cmd $*" >> "${0%/*}/../calls.txt"
case "$1 $2" in
  "package query-activities") printf 'com.android.settings/.Settings\ncom.example.app/com.example.app.Main\ncom.example.app/.Other\n' ;;
  "package resolve-activity") echo "priority=0"; echo "com.example.app/.Main" ;;
  "activity start-activity") echo "Starting: Intent { cmp=$4 }" ;;
esac
"""

FAKE_INPUT = r"""#!/bin/sh
echo "input $*" >> "${0%/*}/../calls.txt"
"""


def write_exec(path, text):
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o755)


def make_image(src, img, size_mb):
    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", "-d", src, img, f"{size_mb}M"], check=True,
                   stdout=subprocess.DEVNULL)


def build_images(top, inst, audio_hal_class=1, sensors_libs=True, radio_libs=True):
    sysd = os.path.join(top, "sys")
    shutil.rmtree(sysd, ignore_errors=True)
    for d in ("system/bin", "system/etc/init/hw", "system/apex/com.android.art/bin", "system/product/etc", "apex", "dev",
              "vendor", "proc", "sys", "data"):
        os.makedirs(os.path.join(sysd, d), exist_ok=True)
    write_exec(os.path.join(sysd, "system/bin/init"), "#!/bin/sh\n")
    open(os.path.join(sysd, "system/build.prop"), "w").write(
        "ro.system.product.cpu.abilist=arm64-v8a,armeabi-v7a,armeabi\n"
        "ro.system.product.cpu.abilist32=armeabi-v7a,armeabi\n"
        "ro.zygote=zygote64_32\nro.build.id=TEST\nro.build.version.release=13\n"
        "ro.build.fingerprint=waydroid/lineage_waydroid_arm64/waydroid_arm64:13/TEST/1:userdebug/test-keys\n")
    open(os.path.join(sysd, "system/etc/init/hw/init.zygote64_32.rc"), "w").write(
        "service zygote /system/bin/app_process64\n    class main\n\nservice zygote_secondary /system/bin/app_process32\n    class main\n")
    if sensors_libs:  # the image's HIDL interface libraries for the sensors HAL
        os.makedirs(os.path.join(sysd, "system/lib64"), exist_ok=True)
        for v in ("2.0", "2.1"):
            open(os.path.join(sysd, f"system/lib64/android.hardware.sensors@{v}.so"), "w").write(f"image sensors {v}\n")
    open(os.path.join(sysd, "init.environ.rc"), "w").write(
        "on early-init\n    export ANDROID_ROOT /system\n    export BOOTCLASSPATH /apex/a.jar:/apex/b.jar\n")
    vend = os.path.join(top, "vend")
    shutil.rmtree(vend, ignore_errors=True)
    os.makedirs(os.path.join(vend, "bin/hw"), exist_ok=True)
    elf = lambda cls: b"\x7fELF" + bytes([cls]) + b"\0" * 59  # ELFCLASS32 or 64
    for name, cls in (("android.hardware.audio.service", audio_hal_class), ("android.hardware.media.omx@1.0-service", 1),
                      ("android.hardware.light@2.0-service", 2)):
        with open(os.path.join(vend, "bin/hw", name), "wb") as f:
            f.write(elf(cls))
    with open(os.path.join(sysd, "system/bin/mediaserver"), "wb") as f:
        f.write(elf(1))
    open(os.path.join(sysd, "system/etc/init/mediaserver.rc"), "w").write("service media /system/bin/mediaserver\n    class main\n")
    open(os.path.join(sysd, "system/etc/init/vold.rc"), "w").write(
        "service vold /system/bin/vold \\\n        --blkid_context=u:r:blkid:s0\n    class core\n\non post-fs\n    start vold\n")
    open(os.path.join(sysd, "system/etc/init/init64.rc"), "w").write("service ok /system/bin/init\n    class main\n")
    os.makedirs(os.path.join(vend, "etc/init"), exist_ok=True)
    os.makedirs(os.path.join(vend, "etc/vintf/manifest"), exist_ok=True)
    for rc, svc, name in (("audio.rc", "vendor.audio-hal", "android.hardware.audio.service"),
                          ("omx.rc", "vendor.media.omx", "android.hardware.media.omx@1.0-service"),
                          ("light.rc", "vendor.light", "android.hardware.light@2.0-service")):
        open(os.path.join(vend, "etc/init", rc), "w").write(f"service {svc} /vendor/bin/hw/{name}\n    class hal\n")
    hal = lambda n: f"    <hal format=\"hidl\">\n        <name>{n}</name>\n    </hal>\n"
    open(os.path.join(vend, "etc/vintf/manifest.xml"), "w").write(
        "<manifest>\n" + hal("android.hardware.media.omx") + hal("android.hardware.light") + "</manifest>\n")
    open(os.path.join(vend, "etc/vintf/manifest/omx.xml"), "w").write("<manifest>\n" + hal("android.hardware.media.omx") + "</manifest>\n")
    open(os.path.join(vend, "build.prop"), "w").write(
        "ro.vendor.product.cpu.abilist=arm64-v8a,armeabi-v7a\nro.vendor.x=1\nro.board.platform=waydroid\n")
    os.makedirs(os.path.join(vend, "lib64/hw"))
    open(os.path.join(vend, "lib64/hw/gralloc.default.so"), "w").write("default gralloc\n")
    open(os.path.join(vend, "lib64/hw/hwcomposer.waydroid.so"), "w").write("hwc\n")
    os.makedirs(os.path.join(vend, "etc/selinux"), exist_ok=True)
    open(os.path.join(vend, "etc/selinux/vendor_property_contexts"), "w").write("ro.hardware. u:object_r:vendor_default_prop:s0\n")
    open(os.path.join(sysd, "system/etc/init/init.waydroid.rc"), "w").write("on boot\n")
    os.makedirs(os.path.join(sysd, "system/product/app/Jelly"), exist_ok=True)
    open(os.path.join(sysd, "system/product/app/Jelly/Jelly.lineage.apk"), "w").write("jelly\n")
    os.makedirs(os.path.join(sysd, "system/product/app/Etar"), exist_ok=True)
    open(os.path.join(sysd, "system/product/app/Etar/Etar.apk"), "w").write("etar\n")
    if radio_libs:  # the image's radio@1.0 and 1.1, which the RIL uses with the emulator's newer ones
        for v in ("1.0", "1.1"):
            open(os.path.join(vend, f"lib64/android.hardware.radio@{v}.so"), "w").write(f"image radio {v}\n")
    make_image(sysd, os.path.join(inst, "system.img"), 16)
    make_image(vend, os.path.join(inst, "vendor.img"), 8)


RADIO_LIBS = ["rild", "ril_cuttlefish", "ril_modem", "ril_compat", "ril_cf_fs", "ril_cf_utils"] + \
    [f"ril_radio_1_{v}" for v in range(2, 7)] + [f"ril_config_1_{v}" for v in range(4)] + ["ril_deprecated", "ril_shim", "telephony_overlay"]


def build_libs(lib):
    os.makedirs(os.path.join(lib, "fakebin"))
    write_exec(os.path.join(lib, "libtawcroot.so"), FAKE_TAWCROOT)
    write_exec(os.path.join(lib, "libbinderd.so"), FAKE_BINDERD)
    write_exec(os.path.join(lib, "libvine_pulse.so"), FAKE_PULSE)
    write_exec(os.path.join(lib, "libvine_wl.so"), FAKE_WL)
    write_exec(os.path.join(lib, "libvine_lmkd.so"), "#!/bin/sh\n# lmkd\n")
    write_exec(os.path.join(lib, "libvine_vold_shim.so"), "vold shim\n")
    write_exec(os.path.join(lib, "libvine_gralloc.so"), "gralloc\n")
    write_exec(os.path.join(lib, "libvine_hal_service.so"), "#!/bin/sh\n# hal\n")
    for part in ("multihal", "ranchu", "wakelock", "shim"):
        write_exec(os.path.join(lib, f"libvine_sensors_{part}.so"), f"sensors {part}\n")
    write_exec(os.path.join(lib, "libvine_sensors_ranchu.so"), "sensors ranchu\0Goldfish Light sensor\0x\n")
    for part in RADIO_LIBS:
        write_exec(os.path.join(lib, f"libvine_{part}.so"), f"radio {part}\n")
    write_exec(os.path.join(lib, "libvine_rild.so"), "radio rild\0libcuttlefish-ril-2.so\0\n")
    write_exec(os.path.join(lib, "libvine_wifinet_overlay.so"), "wifinet overlay\n")
    write_exec(os.path.join(lib, "libvine_modem.so"), "#!/bin/sh\nprintf '%s\\n' \"$@\" > modem.args\nexec sleep 1000\n")
    write_exec(os.path.join(lib, "fakebin/cmd"), FAKE_CMD)
    write_exec(os.path.join(lib, "fakebin/input"), FAKE_INPUT)


class Helper:
    def __init__(self, base, lib, debug_socket):
        self.p = subprocess.Popen([helper_bin, "--rootless", "--base", base, "--libdir", lib, "--debug-socket", debug_socket],
                                  stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
        self.replies = {}
        self.events = queue.Queue()
        self.n = 0
        self.cv = threading.Condition()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.p.stdout:
            f = line.rstrip("\n").split("\t")
            if f[0] == "*":
                self.events.put([f[1]] + [unesc(x) for x in f[2:]])
            else:
                with self.cv:
                    self.replies[f[0]] = (f[1] == "ok", unesc(f[2]) if len(f) > 2 else "")
                    self.cv.notify_all()

    def call(self, cmd, *args, timeout=30):
        self.n += 1
        rid = str(self.n)
        self.p.stdin.write("\t".join([rid, cmd] + [esc(a) for a in args]) + "\n")
        self.p.stdin.flush()
        end = time.time() + timeout
        with self.cv:
            while rid not in self.replies:
                if time.time() > end:
                    return False, "TIMEOUT"
                self.cv.wait(0.2)
            return self.replies.pop(rid)

    def fire(self, cmd, *args):
        self.p.stdin.write("\t".join(["-", cmd] + [esc(a) for a in args]) + "\n")
        self.p.stdin.flush()

    def wait_state(self, instance, state, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            try:
                ev = self.events.get(timeout=0.2)
            except queue.Empty:
                continue
            if ev[0] == "state" and ev[1] == instance and ev[2] == state:
                return ev
        return None


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    with open(f"/proc/{pid}/stat") as f:
        return f.read().rsplit(")", 1)[1].split()[0] != "Z"


def procs_using(path):
    found = []
    for p in os.listdir("/proc"):
        if p.isdigit() and int(p) != os.getpid():
            try:
                with open(f"/proc/{p}/cmdline", "rb") as f:
                    if path.encode() in f.read():
                        found.append(int(p))
            except OSError:
                pass
    return found


def wait_for(cond, timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


def main():
    top = tempfile.mkdtemp(prefix="vine-rootless-")
    base = os.path.join(top, "d" * 40, "files", "instances")
    name = "0123abcd-4567-89ef-0123-456789abcdef"
    inst = os.path.join(base, name)
    lib = os.path.join(top, "lib")
    os.makedirs(inst)
    build_images(top, inst)
    build_libs(lib)
    W = os.path.join(inst, "rootless")
    R = os.path.join(W, "rootfs")
    check(len(os.path.join(W, "run/pulse/native")) > 108, "the socket paths are longer than sun_path")

    sock = os.path.join(top, "debug.sock")
    h = Helper(base, lib, sock)
    print("[hello]")
    ok, text = h.call("hello")
    check(ok and "engine=rootless" in text and f"uid={os.getuid()}" in text, "hello: " + text)
    ok, text = h.call("start", name, os.path.join(top, "elsewhere"))
    check(not ok and "outside" in text, "start outside base refused: " + text)

    print("[boot]")
    ok, text = h.call("start", name, inst, "width=640", "height=480", "dpi=240", "refresh=90", "ram_mb=1536",
                      "sim_operator=46000", "sim_name=中国移动", "net_operator=46000", "net_name=CHINA MOBILE")
    check(ok, "start: " + text)
    check(h.wait_state(name, "running") is not None, "reaches running")
    ok, text = h.call("status", name)
    check(ok and "state=running" in text and "boot=1" in text, "status: " + text)
    ok, text = h.call("start", name, inst)
    check(not ok and "already" in text, "second start refused: " + text)

    print("[rootfs preparation]")
    prop = read(os.path.join(R, "system/build.prop"))
    check("ro.system.product.cpu.abilist=arm64-v8a\n" in prop and "ro.system.product.cpu.abilist32=\n" in prop and
          "ro.zygote=zygote64\n" in prop, "64-bit-only system props: " + prop)
    pixel5 = "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys"
    check("ro.build.id=TQ3A.230901.001.C2\n" in prop and "ro.build.fingerprint=" + pixel5 + "\n" in prop and
          "ro.build.version.release=13\n" in prop, "Pixel 5 build: " + prop)
    check("ro.product.board=redfin\n" in prop and "ro.soc.model=SM7250\n" in prop and
          "ro.vendor.build.fingerprint=" + pixel5 + "\n" in prop and "ro.product.system.model=mainline\n" in prop,
          "Pixel 5 props the image lacks, added: " + prop)
    vprop = read(os.path.join(R, "vendor/build.prop"))
    check("ro.vendor.product.cpu.abilist=arm64-v8a\n" in vprop and "ro.board.platform=lito\n" in vprop, "vendor props: " + vprop)
    check(read(os.path.join(R, "vendor/lib64/hw/hwcomposer.redfin.so")) == "hwc\n" and
          not os.path.exists(os.path.join(R, "vendor/lib64/hw/hwcomposer.waydroid.so")), "HAL found by ro.hardware")
    check(not os.path.exists(os.path.join(R, "system/product/app/Etar")), "Lineage apps removed")
    hidden = read(os.path.join(W, "tawcroot/app-hidden")).split("\n")
    check("/system/etc/init/init.waydroid.rc" in hidden and "/system/product/etc/vine" in hidden and
          "/product/etc/vine" in hidden and "/.tawcroot-tcp-v1" in hidden and "/run" in hidden and
          "/vendor/lib64/hw/hwcomposer.redfin.so" not in hidden and
          not any("Jelly" in p for p in hidden), "Waydroid's files hidden from apps: " + str(hidden))
    ctx = read(os.path.join(R, "vendor/etc/selinux/vendor_property_contexts"))
    check(ctx.startswith("ro.hardware. ") and ctx.count("\nwaydroid. ") == 1 and "\nro.lineage. " in ctx and
          ctx.count("\nro.vendor.display.gpu ") == 1, "Waydroid and Lineage props hidden from apps: " + ctx)
    z = read(os.path.join(R, "system/etc/init/hw/init.zygote64.rc"))
    check("service zygote " in z and "zygote_secondary" not in z, "zygote64 rc: " + z)
    rc_path = os.path.join(R, "system/product/etc/init/vine-rootless.rc")
    rc = read(rc_path)
    check("VIRTGPU" not in rc and os.readlink(os.path.join(R, "dev/socket/kgsl-0")) == "/run/user/1000/kumquat-gpu-0" and
          os.readlink(os.path.join(R, "dev/socket/gralloc")) == "/run/user/1000/gralloc-0",
          "GPU sockets found without environment variables: " + rc)
    check("service lmkd /system/bin/vine_lmkd --budget-mb 1536\n" in rc and "vine_hal_service" in rc and
          "write /run/user/1000/boot_completed 1" in rc, "vine-rootless.rc")
    check(os.path.exists(rc_path) and stat.S_IMODE(os.stat(rc_path).st_mode) == 0o644, "rc mode 644")
    check(os.access(os.path.join(R, "vendor/bin/hw/vine_hal_service"), os.X_OK), "hal service installed")
    check(os.access(os.path.join(R, "system/bin/vine_lmkd"), os.X_OK), "lmkd installed")
    vold = read(os.path.join(R, "system/etc/init/vold.rc"))
    check(vold.startswith("service vold /system/bin/vold \\\n        --blkid_context=u:r:blkid:s0\n"
                          "    setenv LD_PRELOAD /system/lib64/libvine_vold_shim.so\n    class core\n"), "vold preloads the FUSE shim: " + vold)
    check(os.path.exists(os.path.join(R, "system/lib64/libvine_vold_shim.so")), "vold shim installed")
    check("mount none /data/media /mnt/user/0/emulated bind rec\n" in rc, "storage binds")
    check(read(os.path.join(R, "vendor/lib64/hw/gralloc.default.so")) == "gralloc\n" and
          read(os.path.join(R, "vendor/lib64/hw/gralloc.default_real.so")) == "default gralloc\n", "gralloc shim installed")
    check("ro.sf.lcd_density=240" in read(os.path.join(R, "vendor/waydroid.prop")), "waydroid.prop density")
    check(os.access(os.path.join(R, "vendor/bin/hw/android.hardware.sensors@2.1-service.multihal"), os.X_OK) and
          read(os.path.join(R, "vendor/lib64/hw/sensors.ssc.so")) == "sensors ranchu\0TMD3719 Ambient Light\0x\n" and
          read(os.path.join(R, "vendor/lib64/android.hardware.sensors@2.0-ScopedWakelock.so")) == "sensors wakelock\n" and
          read(os.path.join(R, "vendor/lib64/libsensorssc_ext.so")) == "sensors shim\n", "sensors HAL installed, a Pixel's names")
    check(read(os.path.join(R, "vendor/lib64/android.hardware.sensors@2.1.so")) == "image sensors 2.1\n" and
          read(os.path.join(R, "vendor/lib64/android.hardware.sensors@2.0.so")) == "image sensors 2.0\n",
          "sensors interface libraries are the image's")
    check(read(os.path.join(R, "vendor/etc/sensors/hals.conf")) == "sensors.ssc.so\n", "hals.conf")
    sensors_rc = read(os.path.join(R, "vendor/etc/init/android.hardware.sensors@2.1-service-multihal.rc"))
    check("service vendor.sensors-hal-2-1-multihal /vendor/bin/hw/android.hardware.sensors@2.1-service.multihal\n" in sensors_rc and
          "setenv LD_PRELOAD /vendor/lib64/libsensorssc_ext.so\n" in sensors_rc and "rlimit" not in sensors_rc, "sensors rc")
    check("<version>2.1</version>" in read(os.path.join(R, "vendor/etc/vintf/manifest/android.hardware.sensors@2.1-multihal.xml")),
          "sensors VINTF")
    check(read(os.path.join(R, "vendor/bin/hw/rild")) == "radio rild\0libril-qc-hal-qmi.so\0\0\0\n", "rild loads the RIL by its new name")
    check(os.access(os.path.join(R, "vendor/bin/hw/rild"), os.X_OK) and
          read(os.path.join(R, "vendor/lib64/libril-qc-hal-qmi.so")) == "radio ril_cuttlefish\n" and
          read(os.path.join(R, "vendor/lib64/hw/android.hardware.radio-library.compat.so")) == "radio ril_compat\n" and
          read(os.path.join(R, "vendor/lib64/android.hardware.radio@1.6.so")) == "radio ril_radio_1_6\n" and
          read(os.path.join(R, "vendor/lib64/android.hardware.radio.deprecated@1.0.so")) == "radio ril_deprecated\n",
          "RIL installed")
    check(read(os.path.join(R, "vendor/lib64/android.hardware.radio@1.0.so")) == "image radio 1.0\n", "radio@1.0 is the image's")
    check(read(os.path.join(R, "vendor/overlay/framework-res__auto_generated_rro_vendor.apk")) == "radio telephony_overlay\n", "telephony overlay installed")
    radio_rc = read(os.path.join(R, "vendor/etc/init/rild.rc"))
    check(radio_rc.startswith("service vendor.ril-daemon /vendor/bin/hw/rild -l /vendor/lib64/libril-qc-hal-qmi.so\n")
          and "user radio\n" in radio_rc and "setenv LD_PRELOAD /vendor/lib64/libril-qcril-ext.so\n" in radio_rc and
          "setenv RIL_IMPL_NAME \"Qualcomm RIL 1.0\"\n" in radio_rc and "setenv RIL_BASEBAND_VERSION g7250-" in radio_rc and
          read(os.path.join(R, "vendor/lib64/libril-qcril-ext.so")) == "radio ril_shim\n", "RIL rc and shim: " + radio_rc)
    radio_vintf = read(os.path.join(R, "vendor/etc/vintf/manifest/manifest_android.hardware.radio.xml"))
    check(radio_vintf.count("<hal format=\"aidl\">") == 7 and "<fqname>IRadioSim/slot1</fqname>" in radio_vintf and
          "<fqname>IRadioConfig/default</fqname>" in radio_vintf, "RIL VINTF")
    check("android.hardware.telephony" in read(os.path.join(R, "vendor/etc/permissions/android.hardware.telephony.gsm.xml")), "telephony feature")
    check("ro.com.android.mobiledata=false\n" in read(os.path.join(R, "vendor/waydroid.prop")), "mobile data off")
    seed = read(os.path.join(W, "device.seed")).strip()
    check(re.fullmatch(r"[0-9a-f]{32}", seed) is not None, "device seed: " + seed)
    check(re.fullmatch(r"androidboot\.hardware=redfin androidboot\.serialno=0[9AB]\d\d1FDD400\d[0-9A-Z]{2} .*\n",
                       read(os.path.join(W, "tawcroot/cmdline"))) is not None, "serial on the cmdline: " + read(os.path.join(W, "tawcroot/cmdline")))
    mac = read(os.path.join(W, "tawcroot/mac"))
    check(re.fullmatch(r"[0-9a-f]{2}(:[0-9a-f]{2}){5}\n", mac) is not None and int(mac[:2], 16) & 3 == 2, "wlan0 MAC: " + mac)
    boot_id = read(os.path.join(W, "tawcroot/boot_id"))
    check(re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\n", boot_id) is not None and
          boot_id != read("/proc/sys/kernel/random/boot_id"), "guest boot_id: " + boot_id)
    check(read(os.path.join(W, "modem.args")).split("\n")[:-1] ==
          ["--socket", "run/vine-modem", "--seed", seed, "--sim-operator", "46000", "--sim-name", "中国移动",
           "--net-operator", "46000", "--net-name", "CHINA MOBILE"], "vine_modem gets the phone's SIM")
    check("<op n=\"24\"" in read(os.path.join(W, "data/system/appops.xml")), "appops seeded")
    for d in ("data/misc/ethernet", "data/misc/apexdata/com.android.tethering/misc/ethernet"):
        with open(os.path.join(W, d, "ipconfig.txt"), "rb") as f:
            ipc = f.read()
        check(ipc.startswith(b"\0\0\0\3\0\x0cipAssignment\0\x06STATIC\0\x0blinkAddress\0\x0e192.168.31.105\0\0\0\x18") and
              ipc.endswith(b"\0\x02id\0\x05wlan0\0\x03eos"), "ethernet ipconfig: " + repr(ipc))
    check(read(os.path.join(R, "vendor/overlay/ServiceConnectivityResources__auto_generated_rro_vendor.apk")) == "wifinet overlay\n", "wlan0 reported as Wi-Fi: overlay installed")
    check(os.readlink(os.path.join(R, "dev/binder")) == "binderfs/binder", "binder symlink")
    check(os.readlink(os.path.join(R, "apex/com.android.art")) == "/system/apex/com.android.art", "apex symlink")
    check(os.path.isdir(os.path.join(R, "dev/input")) and os.path.isfile(os.path.join(R, "dev/kmsg")), "dev dirs")
    check(os.path.getsize(os.path.join(W, "tawcroot/owners")) > 0, "ownership table")
    init_dir = os.path.join(R, "vendor/etc/init")
    check(sorted(os.listdir(init_dir)) == ["android.hardware.sensors@2.1-service-multihal.rc", "audio.rc", "light.rc", "rild.rc"], "32-bit omx rc dropped: " + str(os.listdir(init_dir)))
    man = read(os.path.join(R, "vendor/etc/vintf/manifest.xml"))
    check("media.omx" not in man and "<name>android.hardware.light</name>" in man and man.count("<hal") == 1, "manifest: " + man)
    check("media.omx" not in read(os.path.join(R, "vendor/etc/vintf/manifest/omx.xml")), "manifest fragment")
    check("service statsd" in rc, "statsd disabled")
    sys_init = os.listdir(os.path.join(R, "system/etc/init"))
    check("mediaserver.rc" not in sys_init and "init64.rc" in sys_init, "32-bit system service dropped: " + str(sys_init))
    check("--create 640x480" in read(os.path.join(W, "wl-args.txt")), "display size")
    check("--refresh 90" in read(os.path.join(W, "wl-args.txt")), "frame rate")
    check(os.path.isfile(os.path.join(inst, "shared/vinefb")), "vinefb created")
    check("/system/bin" in read(os.path.join(W, "init-env.txt")), "init gets the guest PATH")

    print("[input, exec, apps, launch]")
    h.fire("agent_send", name, "touch 0 0 1 0 0.5 0.5")
    h.fire("agent_send", name, "key 4 1")
    h.fire("agent_send", name, "key 187 1")  # APP_SWITCH: no evdev code
    h.fire("agent_send", name, "key 187 0")
    wl = os.path.join(W, "wl-lines.txt")
    check(wait_for(lambda: "key 4 1" in read(wl)), "input lines reach vine_wl: " + read(wl))
    check("touch 0 0 1 0 0.5 0.5" in read(wl) and "187" not in read(wl), "touch passed, APP_SWITCH not")
    calls = os.path.join(lib, "calls.txt")
    check(wait_for(lambda: "input keyevent 187" in read(calls)), "APP_SWITCH through input: " + read(calls))
    check(read(calls).count("keyevent 187") == 1, "only on key down")
    ok, text = h.call("agent", name, "1500", "ping")
    check(ok, "agent ping: " + text)
    ok, text = h.call("exec", name, "5000", "echo", "a b", "c")
    check(ok and text == "exit=0\na b c\n", "exec: " + repr(text))
    ok, text = h.call("exec", name, "5000", "sh", "-c", "exit 7")
    check(ok and text.startswith("exit=7"), "exec exit code: " + repr(text))
    ok, text = h.call("exec", name, "300", "sleep", "5")
    check(ok and text.startswith("exit=-1"), "exec timeout: " + repr(text))
    ok, text = h.call("apps", name)
    check(ok and text == "com.android.settings\tcom.android.settings\ncom.example.app\tcom.example.app\n", "apps: " + repr(text))
    ok, text = h.call("launch", name, "com.example.app")
    check(ok, "launch: " + text)
    check("activity start-activity -n com.example.app/.Main" in read(calls), "launch component: " + read(calls))
    ok, text = h.call("launch", name, "bad;name")
    check(not ok, "bad package refused")
    r = subprocess.run([helper_bin, "--send", sock, "exec", name, "5000", "echo", "from adb"], capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout == "exit=0\nfrom adb\n", "debug socket exec: " + repr(r.stdout + r.stderr))
    r = subprocess.run([helper_bin, "--send", sock, "launch", name, "bad;name"], capture_output=True, text=True)
    check(r.returncode == 1 and "invalid" in r.stdout, "debug socket error: " + repr(r.stdout))
    ok, text = h.call("diag", name)
    check(ok and "Engine: rootless" in text, "diag")

    print("[pause, stop]")
    orphan = int(read(os.path.join(W, "orphan.pid")).strip() or 0)
    check(orphan > 0 and alive(orphan), "orphaned guest running")
    ok, _ = h.call("pause", name)
    check(ok and h.wait_state(name, "paused") is not None, "pause")
    ok, _ = h.call("resume", name)
    check(ok and h.wait_state(name, "running") is not None, "resume")
    ok, text = h.call("stop", name)
    check(ok and text == "stopped", "stop: " + text)
    check(wait_for(lambda: not alive(orphan)), "orphaned guest killed")

    print("[restart keeps the rootfs, init crash]")
    marker = os.path.join(R, "system/kept")
    open(marker, "w").write("x")
    open(os.path.join(W, "init-crash"), "w").write("")
    ok, text = h.call("start", name, inst)
    check(ok, "restart: " + text)
    ev = h.wait_state(name, "error")
    check(ev is not None and "init exited with 3" in ev[3], "init crash reported: " + str(ev))
    check(os.path.exists(marker), "no second extraction")
    check(read(os.path.join(R, "system/etc/init/vold.rc")).count("libvine_vold_shim") == 1, "vold.rc patched once")
    check(read(os.path.join(R, "system/build.prop")).count("\nro.product.board=") == 1, "Pixel 5 props added once")
    check(read(os.path.join(W, "device.seed")).strip() == seed and read(os.path.join(W, "tawcroot/mac")) == mac,
          "restart keeps the device")
    check(read(os.path.join(W, "tawcroot/boot_id")) != boot_id, "restart draws a new boot_id")
    os.unlink(os.path.join(W, "init-crash"))

    print("[reset makes a new device]")
    cmdline = read(os.path.join(W, "tawcroot/cmdline"))
    os.unlink(os.path.join(W, "rootfs.stamp"))
    ok, text = h.call("start", name, inst)
    check(ok and h.wait_state(name, "running") is not None, "reset boots: " + text)
    seed2 = read(os.path.join(W, "device.seed")).strip()
    check(seed2 != seed and re.fullmatch(r"[0-9a-f]{32}", seed2) is not None, "new seed: " + seed2)
    check(read(os.path.join(W, "tawcroot/mac")) != mac and read(os.path.join(W, "tawcroot/cmdline")) != cmdline,
          "new MAC and serial")
    check(read(os.path.join(W, "modem.args")).split("\n")[3] == seed2, "IMEI follows the new seed")
    ok, _ = h.call("stop", name)
    check(ok, "stop after reset")

    print("[64-bit audio HAL kept]")
    name64 = "fedcba98-4567-89ef-0123-456789abcdef"
    inst64 = os.path.join(base, name64)
    os.makedirs(inst64)
    build_images(top, inst64, audio_hal_class=2, sensors_libs=False, radio_libs=False)
    ok, text = h.call("start", name64, inst64)
    check(ok and h.wait_state(name64, "running") is not None, "64-bit image boots: " + text)
    R64 = os.path.join(inst64, "rootless/rootfs")
    check(not os.path.exists(os.path.join(R64, "vendor/bin/hw/vine_hal_service")), "no own audio HAL")
    check("vine_hal_service" not in read(os.path.join(R64, "system/product/etc/init/vine-rootless.rc")), "rc keeps the image's")
    check(not os.path.exists(os.path.join(R64, "vendor/etc/init/android.hardware.sensors@2.1-service-multihal.rc")), "no sensors HAL without the image's libraries")
    check(not os.path.exists(os.path.join(R64, "vendor/etc/init/rild.rc")) and
          "mobiledata" not in read(os.path.join(R64, "vendor/waydroid.prop")), "no RIL without the image's radio@1.0")
    check(read(os.path.join(inst64, "rootless/modem.args")).split("\n")[:-1] == ["--socket", "run/vine-modem", "--seed",
                                                                                 read(os.path.join(inst64, "rootless/device.seed")).strip()],
          "no SIM passed: vine_modem without one")
    ok, _ = h.call("stop", name64)
    check(ok, "stop the 64-bit instance")

    print("[quit]")
    ok, _ = h.call("start", name, inst)
    check(ok and h.wait_state(name, "running") is not None, "start again")
    orphan = int(read(os.path.join(W, "orphan.pid")).strip() or 0)
    h.call("quit")
    h.p.wait(timeout=20)
    check(h.p.returncode == 0, "quit exits 0")
    check(wait_for(lambda: not alive(orphan)), "quit stops the guests")
    check(wait_for(lambda: not procs_using(lib)), "no host processes left: " + str(procs_using(lib)))

    shutil.rmtree(top, ignore_errors=True)
    print(f"\n{checks} checks, {failures} failed")
    sys.exit(1 if failures else 0)


main()
