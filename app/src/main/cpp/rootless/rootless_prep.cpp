#include "rootless_prep.h"
#include "ext4_extract.h"
#include "../utils/vine_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vine::rootless {

namespace {

const char* const kStampVersion = "vine-rootfs 1";

bool fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

bool fail_errno(std::string* error, const std::string& what) {
    return fail(error, what + ": " + strerror(errno));
}

// What the stamp records about the images a rootfs came from
std::string image_stamp(const Layout& l) {
    std::string s = std::string(kStampVersion) + "\n";
    for (const char* name : {"system.img", "vendor.img"}) {
        struct stat st{};
        if (stat((l.instance + "/" + name).c_str(), &st) != 0) continue;
        s += std::string(name) + " " + std::to_string((long long)st.st_size) + " " + std::to_string((long long)st.st_mtime) + "\n";
    }
    return s;
}

// n random bytes as lowercase hex, "" if /dev/urandom fails
std::string random_hex(size_t n) {
    std::string bytes(n, '\0');
    FILE* f = fopen("/dev/urandom", "re");
    const bool ok = f && fread(bytes.data(), 1, n, f) == n;
    if (f) fclose(f);
    if (!ok) return "";
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

// A random UUID in boot_id's form, newline-terminated
std::string random_boot_id() {
    std::string h = random_hex(16);
    if (h.empty()) return "";
    h[12] = '4';
    h[16] = "89ab"[h[16] % 4];
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20) + "\n";
}

bool remove_if_exists(const std::string& path, std::string* error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return true;
    return remove_recursive(path) || fail(error, "could not remove " + path);
}

bool write_mode(const std::string& path, const std::string& text, mode_t mode, std::string* error) {
    if (!write_file_atomic(path, text, mode)) return fail(error, "could not write " + path);
    // init skips group/world-writable rc files whatever the umask did
    return chmod(path.c_str(), mode) == 0 || fail_errno(error, "chmod " + path);
}

bool copy_mode(const std::string& src, const std::string& dst, mode_t mode, std::string* error) {
    const std::string tmp = dst + ".tmp";
    if (!copy_file_sparse(src, tmp)) return fail(error, "could not copy " + src);
    if (chmod(tmp.c_str(), mode) != 0 || rename(tmp.c_str(), dst.c_str()) != 0) {
        unlink(tmp.c_str());
        return fail_errno(error, "install " + dst);
    }
    return true;
}

bool symlink_over(const std::string& target, const std::string& path, std::string* error) {
    unlink(path.c_str());
    return symlink(target.c_str(), path.c_str()) == 0 || fail_errno(error, "symlink " + path);
}

bool mkdirs_or_fail(const std::string& path, std::string* error) {
    return mkdirs(path) || fail(error, "could not create " + path);
}

// `rc` with `option` added to `service`'s definition (after its
// backslash-continued first line); unchanged if it has no such service
std::string with_service_option(const std::string& rc, const std::string& service, const std::string& option) {
    const std::string head = "service " + service + " ";
    size_t at = rc.compare(0, head.size(), head) == 0 ? 0 : rc.find("\n" + head);
    if (at == std::string::npos) return rc;
    if (at) ++at;
    size_t end = rc.find('\n', at);
    while (end != std::string::npos && end > 0 && rc[end - 1] == '\\') end = rc.find('\n', end + 1);
    if (end == std::string::npos) return rc + "\n    " + option + "\n";
    return rc.substr(0, end + 1) + "    " + option + "\n" + rc.substr(end + 1);
}

// The rc that adapts init's services to an app sandbox
// On a real device a dead zygote's apps go with it; under tawcroot they
// outlive it as init's children and keep what they held (WebView's data
// dir lock), so the restarted framework's copies of them crash. They are
// init's children that are not init services (init.svc_debug_pid.*, set on
// userdebug builds); init's own child (subcontext) runs /system/bin/init.
const char kReapOrphans[] =
    "#!/system/bin/sh\n"
    "sm=$(getprop init.svc_debug_pid.servicemanager)\n"
    "initpid=$(sed -n 's/^PPid:[[:space:]]*//p' /proc/$sm/status 2>/dev/null)\n"
    "[ -n \"$initpid\" ] || exit 0\n"
    "services=\" $(getprop | sed -n 's/^\\[init\\.svc_debug_pid\\.[^]]*\\]: \\[\\([0-9]*\\)\\]$/\\1/p' | tr '\\n' ' ') \"\n"
    "for d in /proc/[0-9]*; do\n"
    "    p=${d#/proc/}\n"
    "    [ \"$p\" = \"$$\" ] && continue\n"
    "    pp=$(sed -n 's/^PPid:[[:space:]]*//p' $d/status 2>/dev/null)\n"
    "    [ \"$pp\" = \"$initpid\" ] || continue\n"
    "    case \"$services\" in *\" $p \"*) continue ;; esac\n"
    "    case \"$(head -c 16 $d/cmdline 2>/dev/null | tr '\\0' ' ')\" in init\\ *|/system/bin/init*) continue ;; esac\n"
    "    kill -9 $p\n"
    "done\n";

// Now and then Waydroid's hwcomposer fails to register its display HAL
// (-38: libhidl found no /dev/hwbinder from its display thread, while its
// main thread registers IComposer just fine). That thread then ends;
// SurfaceFlinger waits for the HAL forever, system_server for
// SurfaceFlinger, and the guest never leaves the boot screen. A new
// hwcomposer registers it: killed, init restarts it (and SurfaceFlinger).
// SIGKILL, not ctl.restart: init's SIGTERM leaves it "stopping".
const char kCheckDisplay[] =
    "#!/system/bin/sh\n"
    "svc=vendor.hwcomposer-2-1\n"
    "pid=$(getprop init.svc_debug_pid.$svc)\n"
    "[ -n \"$pid\" ] || exit 0\n"
    "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do\n"
    "    lshal list --neat -i 2>/dev/null | grep -q '^vendor.waydroid.display@' && exit 0\n"
    "    [ \"$(getprop init.svc_debug_pid.$svc)\" = \"$pid\" ] || exit 0\n"
    "    sleep 1\n"
    "done\n"
    "log -p w -t vine \"display HAL not registered in 20 s, killing hwcomposer $pid\"\n"
    "kill -9 $pid\n";

// Network validation probes Google by default, unreachable from where
// many phones are: wlan0 stays PARTIAL_CONNECTIVITY, never VALIDATED, and
// DownloadManager's jobs (which need a validated network) wait forever.
// Probe vendor generate_204 endpoints that answer from China instead.
// NetworkMonitor reads the URLs once, when the network comes up, and keeps
// them: write them as soon as SettingsProvider answers, seconds before
// wlan0's network exists (a new instance's first boot included). As root:
// `cmd settings` writes as com.android.shell, which uid 1000 fails to own.
const char kCaptivePortalUrls[] =
    "#!/system/bin/sh\n"
    "i=0\n"
    "until cmd settings put global captive_portal_https_url https://connect.rom.miui.com/generate_204 "
    "2>/dev/null; do\n"
    "    i=$((i + 1)); [ $i -lt 300 ] || exit 1\n"
    "    sleep 0.2\n"
    "done\n"
    "cmd settings put global captive_portal_http_url http://connectivitycheck.platform.hicloud.com/generate_204\n"
    "cmd settings put global captive_portal_fallback_url http://wifi.vivo.com.cn/generate_204\n"
    "cmd settings put global captive_portal_other_fallback_urls http://conn1.oppomobile.com/generate_204\n";

// The Bluetooth name and address a phone's settings hold, which
// BluetoothManagerService reports while the adapter is off. Before
// that service starts on most boots; on the first, on the next.
std::string device_settings_sh(const std::string& bt_mac) {
    return "#!/system/bin/sh\n"
           "i=0\n"
           "until cmd settings put secure bluetooth_name 'Pixel 5' 2>/dev/null; do\n"
           "    i=$((i + 1)); [ $i -lt 300 ] || exit 1\n"
           "    sleep 0.2\n"
           "done\n"
           "cmd settings put secure bluetooth_address " + bt_mac + "\n"
           "cmd settings put secure bluetooth_addr_valid 1\n";
}

std::string rootless_rc(bool own_audio_hal, bool gpu, int ram_mb) {
    std::string rc =
        // lmkd needs PSI or memcg, both closed to apps: vine_lmkd kills
        // by system_server's priorities, within the instance's RAM
        "service lmkd /system/bin/vine_lmkd" + (ram_mb > 0 ? " --budget-mb " + std::to_string(ram_mb) : std::string()) + "\n"
        "    class core\n"
        "    user lmkd\n"
        "    group lmkd system readproc\n"
        "    capabilities DAC_OVERRIDE KILL IPC_LOCK SYS_NICE SYS_RESOURCE\n"
        "    socket lmkd seqpacket+passcred 0660 system system\n"
        "    task_profiles ServiceCapacityLow\n"
        "    override\n"
        "\n"
        // No keychords: init would open the input FIFOs and block until
        // the hwcomposer writes one
        "service bugreport /system/bin/dumpstate -d -p -z\n"
        "    oneshot\n"
        "    disabled\n"
        "    override\n"
        "\n"
        // Waydroid's statsd can't find libstatssocket (APEX library
        // namespaces) and would restart every few seconds (init.rc
        // starts it by name, so `disabled` alone is not enough)
        "service statsd /apex/com.android.os.statsd/bin/statsd\n"
        "    class main\n"
        "    disabled\n"
        "    oneshot\n"
        "    override\n"
        "\n"
        "on early-init\n"
        // The audio HAL's libpulse takes PULSE_RUNTIME_PATH only when it
        // is its own and 0700, or it never connects to vine_pulse
        "    chown audioserver audio /run/user/1000/pulse\n"
        "    chmod 0700 /run/user/1000/pulse\n";
    // Without the GPU: llvmpipe rasterizes in the drawing thread. With its
    // own rasterizer threads, now and then one runs a shader whose JIT code
    // was already released (SEGV_ACCERR in llvmpipe-N), which takes
    // SurfaceFlinger or an app down. Only then: apps see the environment.
    if (!gpu) rc += "    export LP_NUM_THREADS 0\n";
    rc += "\n"
        // Emulated storage without FUSE (libvine_vold_shim fakes vold's
        // FUSE mount): /data/media where FUSE and its pass-through would
        // be, before zygote forks apps that bind /mnt/user/0 onto /storage
        "on post-fs-data\n"
        "    mkdir /mnt/user/0 0710 shell everybody\n"
        "    mkdir /mnt/user/0/emulated 0700 root root\n"
        "    mkdir /mnt/pass_through/0 0710 root media_rw\n"
        "    mkdir /mnt/pass_through/0/emulated 0710 root media_rw\n"
        "    mount none /data/media /mnt/user/0/emulated bind rec\n"
        "    mount none /data/media /mnt/pass_through/0/emulated bind rec\n"
        "    mount none /mnt/user/0 /storage bind rec\n"
        "\n"
        // The engine's boot signal, in its /run/user/1000
        "on property:sys.boot_completed=1\n"
        "    write /run/user/1000/boot_completed 1\n"
        "\n"
        // see kCaptivePortalUrls
        "on property:init.svc.zygote=running\n"
        "    exec_background - root root -- /system/bin/sh /product/etc/vine/captive_portal_urls.sh\n"
        "    exec_background - root root -- /system/bin/sh /product/etc/vine/device_settings.sh\n"
        "\n"
        // system_server died and zygote with it: see kReapOrphans
        "on property:init.svc.zygote=restarting\n"
        "    exec_background - root root -- /system/bin/sh /product/etc/vine/reap_orphans.sh\n"
        "\n"
        // see kCheckDisplay
        "on property:init.svc.vendor.hwcomposer-2-1=running\n"
        "    exec_background - root root -- /system/bin/sh /product/etc/vine/check_display.sh\n"
        "\n"
        // A shell inside init's session (mounts, linker namespaces) for
        // debugging: the engine's exec channel runs outside it, where
        // APEX binaries do not link. `setprop ctl.start vine_debug` runs
        // /data/local/tmp/vine_debug.sh as root.
        "service vine_debug /system/bin/sh /data/local/tmp/vine_debug.sh\n"
        "    user root\n"
        "    group root\n"
        "    seclabel u:r:su:s0\n"
        "    disabled\n"
        "    oneshot\n";
    if (own_audio_hal) {
        // The image's audio HAL service is 32-bit only; its 64-bit
        // implementations are served by our passthrough service
        rc += "\n"
              "service vendor.audio-hal /vendor/bin/hw/vine_hal_service "
              "android.hardware.audio@4.0::IDevicesFactory android.hardware.audio.effect@4.0::IEffectsFactory\n"
              "    class hal\n"
              "    user audioserver\n"
              "    group audio camera drmrpc inet media mediadrm net_bt net_bt_admin net_bw_acct wakelock context_hub\n"
              "    capabilities BLOCK_SUSPEND SYS_NICE\n"
              "    onrestart restart audioserver\n"
              "    override\n";
    }
    return rc;
}

// What Waydroid's host writes with no GPU device: software GLES (ANGLE on
// lavapipe) over memfd-backed gralloc buffers. With the GPU, ANGLE runs
// on gfxstream-vk and gralloc hands out host ColorBuffers. That gralloc is
// gralloc.redfin, found by ro.hardware with ro.hardware.gralloc unset as on
// a Pixel; anything but "default" makes the hwcomposer pass buffers on by
// handle over android_wlegl, so frames stay on the GPU. The Vulkan driver
// goes by the Pixel's name too (apps read these props and map the files).
// zygote must not preload the
// driver, or its kumquat socket leaks into every fork and zygote's fd
// check aborts.
// libEGL finds its driver by ro.hardware.egl, and only for "angle" does it
// load the driver as ANGLE (its platform first; anything else crashes in
// ANGLE's eglGetProcAddress). The image's libEGL reads this property
// instead (kEglStrings), so ro.hardware.egl can name a Pixel's driver.
constexpr const char* kEglDriverProp = "ro.egl.hal.impl";

std::string waydroid_prop(int dpi, bool gpu, bool egl_renamed) {
    return std::string(gpu ? "" : "ro.hardware.gralloc=default\n") +
           (egl_renamed ? "ro.hardware.egl=adreno\n" + std::string(kEglDriverProp) + "=angle\n"
                        : std::string("ro.hardware.egl=angle\n")) +
           (gpu ? "ro.hardware.vulkan=adreno\nro.vendor.display.gpu=1\nro.zygote.disable_gl_preload=true\n" : "ro.hardware.vulkan=lvp\n") +
           "sys.use_memfd=true\n"
           // Codec2 takes linear buffers from DMA-BUF heaps, which apps
           // can't open: gralloc blobs instead (ION | BUFFERQUEUE | BLOB)
           "debug.stagefright.c2-poolmask=851968\n"
           // The phone's own volume sets the loudness; a guest volume
           // below max only attenuates twice
           "ro.config.media_vol_default=15\n"
           "ro.opengles.version=196610\n"
           "ro.sf.lcd_density=" + std::to_string(dpi) + "\n"
           "waydroid.background_start=false\n"
           "waydroid.stub_sensors_hal=1\n";
}

// True for a 32-bit ELF file
// java.io.DataOutputStream's writeUTF / writeInt
void put_utf(std::string& out, const std::string& s) {
    out += char(s.size() >> 8);
    out += char(s.size() & 0xff);
    out += s;
}

void put_int(std::string& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out += char((v >> shift) & 0xff);
}

} // namespace

// The fake wlan0's static configuration, in IpConfigStore's format:
// EthernetTracker brings wlan0 up with it, no DHCP needed. The one DNS
// server is virtual: tawcroot sends its queries to the first address in
// <store>/dns, which the app rewrites whenever the phone's network (or
// VPN) changes. Android 13 has no way to change a static link's DNS at
// run time, so the guest's setting must never need to.
std::string ethernet_ipconfig() {
    std::string out;
    put_int(out, 3);
    put_utf(out, "ipAssignment");
    put_utf(out, "STATIC");
    put_utf(out, "linkAddress");
    put_utf(out, "192.168.31.105");
    put_int(out, 24);
    put_utf(out, "gateway");
    put_int(out, 0); // default route
    put_int(out, 1);
    put_utf(out, "192.168.31.1");
    put_utf(out, "dns");
    put_utf(out, "192.168.31.1");
    put_utf(out, "proxySettings");
    put_utf(out, "NONE");
    put_utf(out, "id");
    put_utf(out, "wlan0");
    put_utf(out, "eos");
    return out;
}

namespace {

bool is_elf32(const std::string& path) {
    const auto head = read_file_prefix(path, 5);
    return head && head->size() == 5 && head->compare(0, 4, "\x7f" "ELF") == 0 && (*head)[4] == 1;
}

// The HAL a vendor service binary serves, from its name:
// android.hardware.media.omx@1.0-service -> android.hardware.media.omx
std::string hal_of_binary(const std::string& path) {
    std::string base = path.substr(path.rfind('/') + 1);
    const size_t at = base.find('@');
    if (at != std::string::npos) return base.substr(0, at);
    const size_t svc = base.find("-service");
    return svc == std::string::npos ? base : base.substr(0, svc);
}

// The manifest without the <hal> entries named `hal`
std::string drop_manifest_hal(const std::string& xml, const std::string& hal) {
    std::string out;
    size_t pos = 0;
    for (;;) {
        const size_t start = xml.find("<hal", pos);
        if (start == std::string::npos) break;
        const size_t end = xml.find("</hal>", start);
        if (end == std::string::npos) break;
        const size_t stop = end + 6;
        const std::string block = xml.substr(start, stop - start);
        const bool drop = block.find("<name>" + hal + "</name>") != std::string::npos;
        size_t line_start = start;
        while (line_start > pos && (xml[line_start - 1] == ' ' || xml[line_start - 1] == '\t')) --line_start;
        out += xml.substr(pos, (drop ? line_start : stop) - pos);
        pos = stop;
        if (drop && pos < xml.size() && xml[pos] == '\n') ++pos;
    }
    return out + xml.substr(pos);
}

bool drop_vendor_hals(const std::string& R, const std::vector<std::string>& hals, std::string* error);

// Services the host can't run (32-bit only binaries, e.g. mediaserver and
// the omx HAL): their rc files go, and for vendor HALs their VINTF entries
// too, so nothing waits for them (system_server waits forever for a
// declared IOmxStore) and init does not restart them over and over.
// `keep` is served by our own service instead.
bool drop_32bit_services(const std::string& R, const std::string& keep, std::string* error) {
    std::vector<std::string> hals;
    for (const char* dir : {"/system/etc/init", "/vendor/etc/init"}) {
    const bool vendor = dir[1] == 'v';
    const std::string init_dir = R + dir;
    if (DIR* d = opendir(init_dir.c_str())) {
        std::vector<std::string> rcs;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, ".rc") == 0) rcs.push_back(n);
        }
        closedir(d);
        for (const auto& n : rcs) {
            const std::string rc = init_dir + "/" + n;
            bool any = false, all32 = true;
            std::vector<std::string> rc_hals;
            for (const auto& line : split_lines(read_file(rc).value_or(""))) {
                char name[128], bin[256];
                if (sscanf(line.c_str(), " service %127s %255s", name, bin) != 2) continue;
                any = true;
                if (!is_elf32(R + bin)) {
                    all32 = false;
                    continue;
                }
                const std::string hal = hal_of_binary(bin);
                if (hal == keep) all32 = false;
                else rc_hals.push_back(hal);
            }
            if (!any || !all32) continue;
            if (unlink(rc.c_str()) != 0) return fail_errno(error, "remove " + rc);
            if (vendor) hals.insert(hals.end(), rc_hals.begin(), rc_hals.end());
        }
    }
    }
    return drop_vendor_hals(R, hals, error);
}

// Removes HALs from the vendor VINTF manifests
bool drop_vendor_hals(const std::string& R, const std::vector<std::string>& hals, std::string* error) {
    if (hals.empty()) return true;
    const std::string vintf = R + "/vendor/etc/vintf";
    std::vector<std::string> manifests = {vintf + "/manifest.xml"};
    if (DIR* d = opendir((vintf + "/manifest").c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') manifests.push_back(vintf + "/manifest/" + e->d_name);
        closedir(d);
    }
    for (const auto& m : manifests) {
        const auto xml = read_file(m);
        if (!xml) continue;
        std::string edited = *xml;
        for (const auto& hal : hals) edited = drop_manifest_hal(edited, hal);
        if (edited != *xml && !write_mode(m, edited, 0644, error)) return false;
    }
    return true;
}

// 64-bit-only zygote: the 64_32 one without its 32-bit secondary
std::string zygote64_rc(const std::string& zygote64_32) {
    const size_t at = zygote64_32.find("\nservice zygote_secondary");
    return at == std::string::npos ? zygote64_32 : zygote64_32.substr(0, at + 1);
}

// The emulator's sensors HAL (scripts/sensors/fetch-hal.sh), fed by
// vine_sensors. Waydroid's own HAL (1.0) has no sensors; it stays, and
// SensorService takes 2.1 first. Without the prebuilts nothing changes.
//
// The interface libraries are the image's own builds, not the
// emulator's: Waydroid's libhidlbase adds a virtual to IBinder, and
// stubs built against AOSP's call the wrong slot.
// `so` with each whole NUL-terminated string `from` in it replaced by
// `to`, no longer, NUL-padded: a prebuilt's .rodata, retouched in place
template <size_t N>
std::string replace_cstrings(std::string so, const std::pair<const char*, const char*> (&strings)[N]) {
    for (const auto& [from, to] : strings) {
        const std::string f = std::string(1, '\0') + from + '\0';
        for (size_t at = so.find(f); at != std::string::npos; at = so.find(f, at + 1)) {
            std::string t = to;
            t.resize(strlen(from), '\0');
            so.replace(at + 1, t.size(), t);
        }
    }
    return so;
}

// gfxstream's Vulkan device and driver names, which ANGLE puts in
// GL_RENDERER: the device as the host GPU's own name ("%.0s" drops the
// transport), the driver as the vendor's
const std::pair<const char*, const char*> kVulkanStrings[] = {
    {"%s (%s)", "%.0s%s"},
    // Compiled as an 8-byte load and a store of its last "m\0", so the new
    // name ends within its first 8 bytes
    {"gfxstream", "Adreno"},
    {"Mesa " "25.3.6 (git-06f9e28304)", ""},
    {"Goldfish GFXStream", ""},
    // Its server, found without environment variables apps would see:
    // set in every process instead of VIRTGPU_KUMQUAT, no override of the
    // socket path, and the default one (17 characters, "0" appended) a
    // link in /dev/socket (prepare_rootfs)
    {"VIRTGPU_KUMQUAT", "ANDROID_ROOT"},
    {"VIRTGPU_KUMQUAT_GPU_SOCKET", ""},
    {"/tmp/kumquat-gpu-", "/dev/socket/kgsl-"},
    // The SONAME, which dlopen(RTLD_NOLOAD) finds the loaded driver by;
    // the HAL in front of it (vulkan_hal_shim.c) is vulkan.adreno.so
    {"libvulkan_gfxstream.so", "libadreno_utils.so"},
};

// The emulator's sensor names and vendor, in the ranchu HAL's .rodata,
// as the Pixel 5's parts: the chips its vendor.img configures
// (etc/sensors/config) under the names its ADSP firmware gives them. Each
// is a whole NUL-terminated string, the new one no longer: apps list the
// sensors, and "Goldfish" names the emulator. The vendor string is one,
// shared by every sensor; sensors_shim.c sets the other parts' vendors
// and the step and significant motion sensors.
const std::pair<const char*, const char*> kSensorNames[] = {
    {"Goldfish 3-axis Accelerometer", "LSM6DSR Accelerometer"},
    {"Goldfish 3-axis Accelerometer Uncalibrated", "LSM6DSR Accelerometer Uncalibrated"},
    {"Goldfish 3-axis Gyroscope", "LSM6DSR Gyroscope"},
    {"Goldfish 3-axis Gyroscope (uncalibrated)", "LSM6DSR Gyroscope Uncalibrated"},
    {"Goldfish 3-axis Magnetic field sensor", "LIS2MDL Magnetometer"},
    {"Goldfish 3-axis Magnetic field sensor (uncalibrated)", "LIS2MDL Magnetometer Uncalibrated"},
    {"Goldfish Proximity sensor", "TCS3701 Proximity"},
    {"Goldfish Light sensor", "TCS3701 Ambient Light"},
    {"Goldfish Pressure sensor", "BMP380 Barometer"},
    {"Goldfish Humidity sensor", "Humidity"},
    {"Goldfish Ambient Temperature sensor", "Ambient Temperature"},
    {"Goldfish Orientation sensor", "Orientation"},
    {"Goldfish Heart rate sensor", "Heart Rate"},
    {"Goldfish wrist tilt gesture sensor", "Wrist Tilt Gesture"},
    {"Goldfish hinge sensor0 (in degrees)", "Hinge Angle 0"},
    {"Goldfish hinge sensor1 (in degrees)", "Hinge Angle 1"},
    {"Goldfish hinge sensor2 (in degrees)", "Hinge Angle 2"},
    {"The Android Open Source Project", "STMicro"},
};

bool install_sensors_hal(const std::string& R, const std::string& lib_dir, std::string* error) {
    const std::string multihal = lib_dir + "/libvine_sensors_multihal.so";
    const std::string ranchu = lib_dir + "/libvine_sensors_ranchu.so";
    const std::string wakelock = lib_dir + "/libvine_sensors_wakelock.so";
    const std::string sys = R + "/system/lib64/";
    for (const std::string& f : {multihal, ranchu, wakelock, lib_dir + "/libvine_sensors_shim.so",
                                 sys + "android.hardware.sensors@2.0.so", sys + "android.hardware.sensors@2.1.so"}) {
        if (!is_regular_file(f)) return true;
    }
    const std::string V = R + "/vendor";
    for (const char* d : {"/bin/hw", "/lib64/hw", "/etc/init", "/etc/sensors", "/etc/vintf/manifest"}) {
        if (!mkdirs_or_fail(V + d, error)) return false;
    }
    const std::string service = "/vendor/bin/hw/android.hardware.sensors@2.1-service.multihal";
    // Earlier names
    for (const char* old : {"/lib64/hw/android.hardware.sensors@2.1-impl.ranchu.so", "/lib64/libvine_sensors_shim.so",
                            "/etc/init/vine-sensors.rc", "/etc/vintf/manifest/vine-sensors.xml"})
        unlink((V + old).c_str());
    const auto ranchu_so = read_file(ranchu);
    if (!ranchu_so) return fail(error, "could not read " + ranchu);
    return copy_mode(multihal, R + service, 0755, error) &&
           write_mode(V + "/lib64/hw/sensors.ssc.so", replace_cstrings(*ranchu_so, kSensorNames), 0644, error) &&
           copy_mode(wakelock, V + "/lib64/android.hardware.sensors@2.0-ScopedWakelock.so", 0644, error) &&
           copy_mode(lib_dir + "/libvine_sensors_shim.so", V + "/lib64/libsensorssc_ext.so", 0644, error) &&
           copy_mode(sys + "android.hardware.sensors@2.0.so", V + "/lib64/android.hardware.sensors@2.0.so", 0644, error) &&
           copy_mode(sys + "android.hardware.sensors@2.1.so", V + "/lib64/android.hardware.sensors@2.1.so", 0644, error) &&
           write_mode(V + "/etc/sensors/hals.conf", "sensors.ssc.so\n", 0644, error) &&
           write_mode(V + "/etc/vintf/manifest/android.hardware.sensors@2.1-multihal.xml",
                      "<manifest version=\"1.0\" type=\"device\">\n"
                      "    <hal format=\"hidl\">\n"
                      "        <name>android.hardware.sensors</name>\n"
                      "        <transport>hwbinder</transport>\n"
                      "        <version>2.1</version>\n"
                      "        <interface>\n"
                      "            <name>ISensors</name>\n"
                      "            <instance>default</instance>\n"
                      "        </interface>\n"
                      "    </hal>\n"
                      "</manifest>\n",
                      0644, error) &&
           // The emulator's rc, less its rtprio rlimit and task profile
           write_mode(V + "/etc/init/android.hardware.sensors@2.1-service-multihal.rc",
                      "service vendor.sensors-hal-2-1-multihal " + service + "\n"
                      "    class hal\n"
                      "    user system\n"
                      "    group system wakelock context_hub uhid\n"
                      "    capabilities BLOCK_SUSPEND\n"
                      "    setenv LD_PRELOAD /vendor/lib64/libsensorssc_ext.so\n",
                      0644, error);
}

// TQ3A.230901.001.C2's radio and bootloader, as its factory image
// (redfin-tq3a.230901.001.c2) names them
constexpr const char* kBaseband = "g7250-00258-230518-B-10157620";
constexpr const char* kBootloader = "r3-0.5-9825705";

// Google's camera HAL with the emulator's sensor HWL
// (scripts/sensors/fetch-hal.py): a back and a front camera that render a
// test scene, configured by the JSON files (without ro.boot.qemu it reads
// them all; the depth camera's is left out, as a Pixel 5 has none). The
// image has no camera HAL; without the prebuilts it gets none.
const std::pair<const char*, const char*> kCameraFiles[] = {
    {"libvine_cam_provider.so", "/bin/hw/android.hardware.camera.provider@2.7-service-google"},
    {"libvine_cam_hal.so", "/lib64/libgooglecamerahal.so"},
    {"libvine_cam_halutils.so", "/lib64/libgooglecamerahalutils.so"},
    {"libvine_cam_hwl.so", "/lib64/libgooglecamerahwl_impl.so"},
    {"libvine_cam_profiler.so", "/lib64/lib_profiler.so"},
    {"libvine_cam_meminfo.so", "/lib64/libmeminfo.so"},
    {"libvine_cam_grallocusage.so", "/lib64/libgrallocusage.so"},
    {"libvine_cam_thermal_2_0.so", "/lib64/android.hardware.thermal@2.0.so"},
    {"libvine_cam_thermal_1_0.so", "/lib64/android.hardware.thermal@1.0.so"},
    {"libvine_cam_back.so", "/etc/config/emu_camera_back.json"},
    {"libvine_cam_front.so", "/etc/config/emu_camera_front.json"},
};

// Interface libraries the HAL needs that the image has only in /system (not
// in the VNDK): copied into /vendor, as a device's vendor partition carries them
const char* const kCameraSystemLibs[] = {
    "android.frameworks.sensorservice@1.0.so", "android.hidl.allocator@1.0.so",
    "android.hardware.camera.provider@2.4.so", "android.hardware.camera.provider@2.5.so",
    "android.hardware.camera.provider@2.6.so", "android.hardware.camera.provider@2.7.so",
    "android.hardware.camera.common@1.0.so",   "android.hardware.camera.device@1.0.so",
    "android.hardware.camera.device@3.2.so",   "android.hardware.camera.device@3.3.so",
    "android.hardware.camera.device@3.4.so",   "android.hardware.camera.device@3.5.so",
    "android.hardware.camera.device@3.6.so",   "android.hardware.camera.device@3.7.so",
    "android.hardware.camera.metadata@3.2.so", "android.hardware.camera.metadata@3.3.so",
    "android.hardware.camera.metadata@3.4.so", "android.hardware.camera.metadata@3.5.so",
    "android.hardware.camera.metadata@3.6.so",
};

// The HAL's configurations: apps have no use for them, and the names say emulator
const char* const kCameraHidden[] = {"/vendor/etc/config/emu_camera_back.json", "/vendor/etc/config/emu_camera_front.json"};

// What the cameras support, in AOSP's feature files under their names (a
// Pixel's /vendor/etc/permissions has the same four)
const std::pair<const char*, const char*> kCameraFeatures[] = {
    {"android.hardware.camera.flash-autofocus.xml",
     "android.hardware.camera\0android.hardware.camera.autofocus\0android.hardware.camera.flash\0"},
    {"android.hardware.camera.front.xml", "android.hardware.camera.front\0"},
    {"android.hardware.camera.full.xml",
     "android.hardware.camera.any\0android.hardware.camera.level.full\0"
     "android.hardware.camera.capability.manual_sensor\0android.hardware.camera.capability.manual_post_processing\0"},
    {"android.hardware.camera.raw.xml", "android.hardware.camera.capability.raw\0"},
};

bool copy_if_changed(const std::string& from, const std::string& to, std::string* error) {
    struct stat a {}, b {};
    if (stat(from.c_str(), &a) != 0) return fail_errno(error, "stat " + from);
    if (stat(to.c_str(), &b) == 0 && a.st_size == b.st_size) return true;
    return copy_mode(from, to, 0644, error);
}

bool install_camera_hal(const std::string& R, const std::string& lib_dir, bool* installed, std::string* error) {
    *installed = false;
    for (const auto& [lib, path] : kCameraFiles) {
        if (!is_regular_file(lib_dir + "/" + lib)) return true;
    }
    const std::string V = R + "/vendor";
    for (const char* d : {"/bin/hw", "/lib64", "/etc/init", "/etc/config", "/etc/vintf/manifest", "/etc/permissions"}) {
        if (!mkdirs_or_fail(V + d, error)) return false;
    }
    for (const auto& [lib, path] : kCameraFiles) {
        if (!copy_mode(lib_dir + "/" + lib, V + path, std::string(path).rfind("/bin/", 0) == 0 ? 0755 : 0644, error))
            return false;
    }
    for (const char* lib : kCameraSystemLibs) {
        const std::string from = R + "/system/lib64/" + lib, to = V + "/lib64/" + lib;
        if (is_regular_file(from) && !is_regular_file(R + "/system/apex/com.android.vndk.current/lib64/" + lib) &&
            !copy_if_changed(from, to, error))
            return false;
    }
    for (const auto& [file, features] : kCameraFeatures) {
        std::string xml = "<permissions>\n";
        for (const char* f = features; *f; f += strlen(f) + 1) xml += std::string("    <feature name=\"") + f + "\" />\n";
        if (!write_mode(V + "/etc/permissions/" + file, xml + "</permissions>\n", 0644, error)) return false;
    }
    // camera_shim.c, under a Pixel 5 camera library's name, when there is one
    const std::string shim = lib_dir + "/libvine_camera_shim.so";
    const bool preload = is_regular_file(shim);
    if (preload && !copy_mode(shim, V + "/lib64/libcamerapostproc.so", 0644, error)) return false;
    if (!write_mode(V + "/etc/vintf/manifest/android.hardware.camera.provider@2.7-service-google.xml",
                    "<manifest version=\"1.0\" type=\"device\">\n"
                    "    <hal format=\"aidl\">\n"
                    "        <name>android.hardware.camera.provider</name>\n"
                    "        <version>1</version>\n"
                    "        <interface>\n"
                    "            <name>ICameraProvider</name>\n"
                    "            <instance>internal/0</instance>\n"
                    "        </interface>\n"
                    "    </hal>\n"
                    "</manifest>\n",
                    0644, error) ||
        // The emulator's rc, less its rtprio rlimit and task profiles
        !write_mode(V + "/etc/init/android.hardware.camera.provider@2.7-service-google.rc",
                    "service vendor.camera-provider-2-7-google /vendor/bin/hw/android.hardware.camera.provider@2.7-service-google\n"
                    "    class hal\n"
                    "    user system\n"
                    "    group system\n"
                    "    capabilities SYS_NICE\n" +
                        std::string(preload ? "    setenv LD_PRELOAD /vendor/lib64/libcamerapostproc.so\n" : ""),
                    0644, error))
        return false;
    *installed = true;
    return true;
}

// The emulator's RIL (scripts/sensors/fetch-hal.py) and its shim, on
// vine_modem's socket, and an overlay that lets the framework use it. The image has none, so telephony sees no SIM without it; without
// the prebuilts nothing changes. Its interface libraries are the
// emulator's but for radio@1.0 and 1.1, which the image has: unlike the
// sensors HAL, the RIL serves the AIDL radio HALs and only uses its HIDL
// types in-process.
// rild dlopens its RIL by this name, whatever -l says
const std::pair<const char*, const char*> kRildStrings[] = {{"libcuttlefish-ril-2.so", "libril-qc-hal-qmi.so"}};

// Installed under a Qualcomm Pixel's names where apps could tell the
// emulator's apart (they can stat these paths and read the rc)
const std::pair<const char*, const char*> kRadioFiles[] = {
    {"libvine_rild.so", "/bin/hw/rild"},
    {"libvine_ril_cuttlefish.so", "/lib64/libril-qc-hal-qmi.so"},
    {"libvine_ril_modem.so", "/lib64/libril-modem-lib.so"},
    {"libvine_ril_compat.so", "/lib64/hw/android.hardware.radio-library.compat.so"},
    {"libvine_ril_cf_fs.so", "/lib64/libcuttlefish_fs.so"},
    {"libvine_ril_cf_utils.so", "/lib64/libcuttlefish_utils.so"},
    {"libvine_ril_radio_1_2.so", "/lib64/android.hardware.radio@1.2.so"},
    {"libvine_ril_radio_1_3.so", "/lib64/android.hardware.radio@1.3.so"},
    {"libvine_ril_radio_1_4.so", "/lib64/android.hardware.radio@1.4.so"},
    {"libvine_ril_radio_1_5.so", "/lib64/android.hardware.radio@1.5.so"},
    {"libvine_ril_radio_1_6.so", "/lib64/android.hardware.radio@1.6.so"},
    {"libvine_ril_config_1_0.so", "/lib64/android.hardware.radio.config@1.0.so"},
    {"libvine_ril_config_1_1.so", "/lib64/android.hardware.radio.config@1.1.so"},
    {"libvine_ril_config_1_2.so", "/lib64/android.hardware.radio.config@1.2.so"},
    {"libvine_ril_config_1_3.so", "/lib64/android.hardware.radio.config@1.3.so"},
    {"libvine_ril_deprecated.so", "/lib64/android.hardware.radio.deprecated@1.0.so"},
    {"libvine_ril_shim.so", "/lib64/libril-qcril-ext.so"},
    // scripts/radio/build-overlay.sh: without it the framework never uses the RIL
    {"libvine_telephony_overlay.so", "/overlay/framework-res__auto_generated_rro_vendor.apk"},
};

// *installed stays false without the prebuilts or the image's radio@1.0 and 1.1
bool install_radio_hal(const std::string& R, const std::string& lib_dir, bool* installed, std::string* error) {
    *installed = false;
    const std::string V = R + "/vendor";
    for (const auto& [lib, path] : kRadioFiles) {
        if (!is_regular_file(lib_dir + "/" + lib)) return true;
    }
    for (const char* f : {"/lib64/android.hardware.radio@1.0.so", "/lib64/android.hardware.radio@1.1.so"}) {
        if (!is_regular_file(V + f)) return true;
    }
    for (const char* d : {"/bin/hw", "/lib64/hw", "/etc/init", "/etc/vintf/manifest", "/etc/permissions", "/overlay"}) {
        if (!mkdirs_or_fail(V + d, error)) return false;
    }
    for (const auto& [lib, path] : kRadioFiles) {
        if (!copy_mode(lib_dir + "/" + lib, V + path, std::string(path).rfind("/bin/", 0) == 0 ? 0755 : 0644, error))
            return false;
    }
    const auto rild = read_file(V + "/bin/hw/rild");
    if (!rild || !write_mode(V + "/bin/hw/rild", replace_cstrings(*rild, kRildStrings), 0755, error))
        return fail(error, "could not retouch rild");
    // Earlier names
    for (const char* old : {"/bin/hw/libgoldfish-rild", "/lib64/libcuttlefish-ril-2.so", "/lib64/libvine_ril_shim.so",
                            "/etc/init/vine-radio.rc", "/etc/vintf/manifest/vine-radio.xml",
                            "/etc/permissions/vine-telephony.xml"})
        unlink((V + old).c_str());
    std::string manifest = "<manifest version=\"1.0\" type=\"device\">\n";
    for (const char* hal : {"config/IRadioConfig/default", "data/IRadioData/slot1", "messaging/IRadioMessaging/slot1",
                            "modem/IRadioModem/slot1", "network/IRadioNetwork/slot1", "sim/IRadioSim/slot1",
                            "voice/IRadioVoice/slot1"}) {
        const std::string h = hal;
        const size_t slash = h.find('/');
        manifest += "    <hal format=\"aidl\">\n        <name>android.hardware.radio." + h.substr(0, slash) +
                    "</name>\n        <fqname>" + h.substr(slash + 1) + "</fqname>\n    </hal>\n";
    }
    manifest += "</manifest>\n";
    if (!write_mode(V + "/etc/vintf/manifest/manifest_android.hardware.radio.xml", manifest, 0644, error) ||
        // The emulator's rc, with the RIL library named (without
        // ro.boot.qemu it is not found) and the shim that leads it to vine_modem
        !write_mode(V + "/etc/init/rild.rc",
                    "service vendor.ril-daemon /vendor/bin/hw/rild -l /vendor/lib64/libril-qc-hal-qmi.so\n"
                    "    class main\n"
                    "    user radio\n"
                    "    group radio cache inet misc audio log readproc wakelock\n"
                    "    setenv LD_PRELOAD /vendor/lib64/libril-qcril-ext.so\n"
                    // The Pixel 5's in place of reference-ril's (see kBaseband)
                    "    setenv RIL_IMPL_NAME \"Qualcomm RIL 1.0\"\n"
                    "    setenv RIL_BASEBAND_VERSION " + std::string(kBaseband) + "\n",
                    0644, error) ||
        // A phone's: apps check for it before they read the SIM
        !write_mode(V + "/etc/permissions/android.hardware.telephony.gsm.xml",
                    "<permissions>\n"
                    "    <feature name=\"android.hardware.telephony\" />\n"
                    "    <feature name=\"android.hardware.telephony.gsm\" />\n"
                    "</permissions>\n",
                    0644, error))
        return false;
    *installed = true;
    return true;
}

// Waydroid's health HAL overwrites what healthd read from
// /sys/class/power_supply with an emulator's battery (AC and USB online,
// 85 %, 3.6 V, 35 °C) in its healthd_board_battery_update(). Patched to
// return at once, so the phone's battery the app publishes there shows.
// Found by its body (mov w9, #0x101; mov w10, #1 after the frame setup);
// already patched or another build: left alone.
bool patch_health_hal(const std::string& R, std::string* error) {
    const std::string path = R + "/vendor/bin/hw/android.hardware.health@2.0-service.waydroid";
    auto bin = read_file(path);
    if (!bin) return true;
    auto words = [](std::initializer_list<uint32_t> ws) {
        std::string out;
        for (uint32_t w : ws)
            for (int i = 0; i < 4; ++i) out += char((w >> (8 * i)) & 0xff);
        return out;
    };
    const std::string body = words({0x52802029, 0x5280002a}), patched = words({0x2a1f03e0, 0xd65f03c0});
    const size_t at = bin->find(body);
    if (at == std::string::npos || at < 12 || bin->find(body, at + 1) != std::string::npos) return true;
    const size_t fn = at - 12;
    if (bin->compare(fn, 8, patched) == 0) return true;
    if (bin->compare(fn, 8, words({0xa9bf7bfd, 0x910003fd})) != 0 ||
        bin->find(words({0x529fbc09, 0x72a00389}), at) > at + 32)  // 1900000 µAh
        return true;
    bin->replace(fn, 8, patched);
    return write_mode(path, *bin, 0755, error);
}

// Apps get an Adreno's GL strings: gles_shim.c takes the place of ANGLE's
// GLES libraries, and the EGL loader finds the GL entry points it lacks
// through ANGLE's EGL. ANGLE's own GLES library moves to an Adreno driver
// library's name, libllvm-glnext.so, beside the EGL library, whose
// DT_NEEDED and the name it dlopen()s are rewritten to it (shorter, in
// place). ro.hardware.egl stays angle: ANGLE's EGL library loaded under
// another name finds no GLES library and every GL process crashes.
const char* const kAngleHidden[] = {"/vendor/lib64/egl/libEGL_mesa.so", "/vendor/lib64/egl/libGLESv1_CM_mesa.so",
                                    "/vendor/lib64/egl/libGLESv2_mesa.so"};

// ANGLE caps GLES at 3.1, and leaves out the geometry and tessellation
// shader extensions 3.2 needs, unless its exposeNonConformantExtensionsAndVersions
// feature is on, which only an override turns on (a property or an
// environment variable apps would see). The guest's Vulkan device has what
// 3.2 needs; with the feature on, ANGLE gives apps a real 3.2.
// In the image's ANGLE (arm64), RendererVk::initFeatures's store of the
// feature's `false` (strb wzr, [x25, #0xe40]) becomes a branch to a few
// instructions in the zero padding after the executable segment, on the
// same mapped page, that store 1 and branch back (x16 is free there).
// Found by its surrounding bytes; the cave must still be zeros.
const std::string kAngleFeatureSite("\x37\x83\x3a\x39\x68\x01\x08\x2a\x3f\x03\x39\x39\x08\x01\x0a\x2a", 16);
constexpr size_t kAngleStoreAt = 8;        // within kAngleFeatureSite
constexpr size_t kAngleCaveDelta = 0x2def48; // from the store to the cave
// An earlier patch that only made getMaxSupportedESVersion read the
// feature as on: ANGLE then made 3.2 contexts without 3.2's shaders
const std::string kAngleReaderOrig("\x00\x00\x09\x8b\xab\xfc\x0a\x94\x88\xc2\x67\x39\xa0\x00\x00\x36\xa8\x00\x00\x35", 20);
const std::string kAngleReaderPatched("\x00\x00\x09\x8b\xab\xfc\x0a\x94\x28\x00\x80\x52\xa0\x00\x00\x36\xa8\x00\x00\x35", 20);

void put_u32(std::string* s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) (*s)[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

uint32_t arm64_b(size_t from, size_t to) {
    return 0x14000000u | (static_cast<uint32_t>((static_cast<int64_t>(to) - static_cast<int64_t>(from)) >> 2) & 0x3ffffffu);
}

bool enable_angle_es32(const std::string& path, std::string* error) {
    auto lib = read_file(path);
    if (!lib) return true;
    bool changed = false;
    const size_t reader = lib->find(kAngleReaderPatched);
    if (reader != std::string::npos) {
        lib->replace(reader, kAngleReaderOrig.size(), kAngleReaderOrig);
        changed = true;
    }
    const size_t site = lib->find(kAngleFeatureSite);
    if (site != std::string::npos && lib->find(kAngleFeatureSite, site + 1) == std::string::npos) {
        const size_t store = site + kAngleStoreAt, cave = store + kAngleCaveDelta;
        if (cave + 12 <= lib->size() && lib->compare(cave, 12, std::string(12, '\0')) == 0) {
            put_u32(&*lib, cave, 0x52800030u);     // mov w16, #1
            put_u32(&*lib, cave + 4, 0x39390330u); // strb w16, [x25, #0xe40]
            put_u32(&*lib, cave + 8, arm64_b(cave + 8, store + 4));
            put_u32(&*lib, store, arm64_b(store, cave));
            changed = true;
        }
    }
    return !changed || write_mode(path, *lib, 0644, error);
}

bool install_gles_shim(const std::string& R, const std::string& lib_dir, std::string* error) {
    // Beside the EGL library, which dlopen()s it from its own directory
    const std::string egl = R + "/vendor/lib64/egl", angle_gles = egl + "/libllvm-glnext.so";
    const std::string earlier = R + "/vendor/lib64/libllvm-glnext.so";
    if (is_regular_file(earlier) && !is_regular_file(angle_gles) && rename(earlier.c_str(), angle_gles.c_str()) != 0)
        return fail_errno(error, "rename " + earlier);
    unlink(earlier.c_str());
    const auto shim = read_file(lib_dir + "/libvine_gles_shim.so");
    const auto gles = read_file(egl + "/libGLESv2_angle.so");
    auto egl_lib = read_file(egl + "/libEGL_angle.so");
    if (!shim || !gles || !egl_lib) return true;
    // The image's library, unless it is already a shim: any build of it, an
    // earlier one included, names the library it loads (gles_shim.c)
    const bool is_shim = gles->find("/vendor/lib64/egl/libllvm-glnext.so") != std::string::npos;
    if (!is_shim && !write_mode(angle_gles, *gles, 0644, error)) return false;
    if (!is_regular_file(angle_gles)) return true;
    if (!enable_angle_es32(angle_gles, error)) return false;
    // DT_NEEDED, and the name it dlopen()s at run time with ".so" appended
    const std::pair<std::string, std::string> names[] = {
        {std::string("libGLESv2_angle.so\0", 19), std::string("libllvm-glnext.so\0\0", 19)},
        {std::string("\0libGLESv2_angle\0", 17), std::string("\0libllvm-glnext\0\0", 17)}};
    bool changed = false;
    for (const auto& [from, to] : names)
        for (size_t at; (at = egl_lib->find(from)) != std::string::npos; changed = true) egl_lib->replace(at, from.size(), to);
    if ((changed && !write_mode(egl + "/libEGL_angle.so", *egl_lib, 0644, error)) ||
        !write_mode(egl + "/libGLESv2_angle.so", *shim, 0644, error) ||
        !write_mode(egl + "/libGLESv1_CM_angle.so", *shim, 0644, error))
        return false;
    for (const char* f : {"/libEGL_adreno.so", "/libGLESv2_adreno.so", "/libGLESv1_CM_adreno.so"}) unlink((egl + f).c_str());
    return true;
}

// tawcroot's device view (tawcroot/include/device_view.h): what apps read
// of the Pixel 5's SoC, kernel, RAM and storage instead of the phone's.
// Unverified on a device: the Kryo 475 silver cores' variant, the kernel's
// build strings, MemTotal and the userdata size (a 128 GB model's).
// An app's /proc/self/mounts on a Pixel 5 (Android 13, ext4 dynamic
// partitions, f2fs userdata, FUSE storage), tawcroot's template for apps'
// mounts and mountinfo, with each APEX the image has mounted twice (by name
// and name@version) as apexd does. Block device numbers and sizes are made
// up, unverified on a device; tawcroot adds the app's own profile mounts.
std::string device_mounts(const std::string& R) {
    const std::string data_opts =
        "rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,"
        "inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,"
        "resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0\n";
    const std::string tmp = "rw,seclabel,nosuid,nodev,noexec,relatime,size=3806154k,nr_inodes=951538,mode=755";
    std::string m =
        "/dev/block/dm-6 / ext4 ro,seclabel,relatime 0 0\n"
        "tmpfs /dev tmpfs rw,seclabel,nosuid,relatime,size=3806154k,nr_inodes=951538,mode=755 0 0\n"
        "devpts /dev/pts devpts rw,seclabel,relatime,mode=600,ptmxmode=000 0 0\n"
        "proc /proc proc rw,relatime,gid=3009,hidepid=invisible 0 0\n"
        "sysfs /sys sysfs rw,seclabel,relatime 0 0\n"
        "selinuxfs /sys/fs/selinux selinuxfs rw,relatime 0 0\n"
        "tmpfs /mnt " + tmp + ",gid=1000 0 0\n"
        "tmpfs /mnt/installer " + tmp + ",gid=1000 0 0\n"
        "tmpfs /mnt/androidwritable " + tmp + ",gid=1000 0 0\n"
        "/dev/block/dm-7 /system_ext ext4 ro,seclabel,relatime 0 0\n"
        "/dev/block/dm-8 /vendor ext4 ro,seclabel,relatime 0 0\n"
        "/dev/block/dm-9 /product ext4 ro,seclabel,relatime 0 0\n"
        "/dev/block/dm-10 /vendor_dlkm ext4 ro,seclabel,relatime 0 0\n"
        "tmpfs /apex " + tmp + " 0 0\n"
        "tmpfs /linkerconfig " + tmp + " 0 0\n"
        "none /dev/blkio cgroup rw,nosuid,nodev,noexec,relatime,blkio 0 0\n"
        "none /sys/fs/cgroup cgroup2 rw,nosuid,nodev,noexec,relatime,memory_recursiveprot 0 0\n"
        "none /dev/cpuctl cgroup rw,nosuid,nodev,noexec,relatime,cpu 0 0\n"
        "none /dev/cpuset cgroup rw,nosuid,nodev,noexec,relatime,cpuset,noprefix,release_agent=/sbin/cpuset_release_agent 0 0\n"
        "none /dev/memcg cgroup rw,nosuid,nodev,noexec,relatime,memory 0 0\n"
        "none /dev/stune cgroup rw,nosuid,nodev,noexec,relatime,schedtune 0 0\n"
        "tracefs /sys/kernel/tracing tracefs rw,seclabel,relatime,gid=3012 0 0\n"
        "pstore /sys/fs/pstore pstore rw,seclabel,nosuid,nodev,noexec,relatime 0 0\n"
        "bpf /sys/fs/bpf bpf rw,nosuid,nodev,noexec,relatime 0 0\n"
        "none /config configfs rw,nosuid,nodev,noexec,relatime 0 0\n"
        "binder /dev/binderfs binder rw,relatime,max=1048576,stats=global 0 0\n"
        "/dev/block/sda10 /metadata ext4 rw,seclabel,nosuid,nodev,noatime,discard,nodelalloc,commit=1,data=journal 0 0\n"
        "/dev/block/dm-38 /data f2fs " + data_opts +
        "tmpfs /data_mirror " + tmp + ",gid=1000 0 0\n";
    for (const char* d : {"/data_mirror/data_ce/null", "/data_mirror/data_de/null", "/data_mirror/misc_ce/null",
                          "/data_mirror/misc_de/null", "/data_mirror/cur_profiles", "/data_mirror/ref_profiles",
                          "/data_mirror/data_ce/null/0", "/data/user/0"})
        m += std::string("/dev/block/dm-38 ") + d + " f2fs " + data_opts;
    std::vector<std::string> apexes;
    if (DIR* d = opendir((R + "/system/apex").c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            // vendor API 30: the VNDK snapshots of older vendors aren't activated
            if (n[0] == '.' || n.find('.') == std::string::npos || n == "com.android.vndk.v28" || n == "com.android.vndk.v29")
                continue;
            apexes.push_back(n);
        }
        closedir(d);
    }
    std::sort(apexes.begin(), apexes.end());
    int dm = 11;
    for (const std::string& n : apexes) {
        uint32_t h = 2166136261u;
        for (unsigned char c : n) h = (h ^ c) * 16777619u;
        // Mainline modules' version form: 33 for T, a train, then build digits
        const std::string version = "33" + std::to_string(1000 + h % 9000) + "000";
        const std::string dev = "/dev/block/dm-" + std::to_string(dm++);
        m += dev + " /apex/" + n + "@" + version + " ext4 ro,dirsync,seclabel,nodev,noatime 0 0\n";
        m += dev + " /apex/" + n + " ext4 ro,dirsync,seclabel,nodev,noatime 0 0\n";
    }
    m += "/dev/fuse /mnt/user/0/emulated fuse rw,lazytime,nosuid,nodev,noexec,noatime,user_id=0,group_id=0,allow_other 0 0\n"
         "/dev/fuse /storage/emulated fuse rw,lazytime,nosuid,nodev,noexec,noatime,user_id=0,group_id=0,allow_other 0 0\n";
    return m;
}

bool write_device_view(const std::string& R, const std::string& store, std::string* error) {
    const std::string D = store + "/device";
    if (!remove_if_exists(D, error) || !mkdirs_or_fail(D, error)) return false;
    struct Core { int first, last; const char *variant, *part, *midr; int min_khz, max_khz; };
    // Kryo 475: six silver cores, one gold, one gold prime
    const Core cores[] = {{0, 5, "0x7", "0x805", "0x00000000517f805e", 300000, 1804800},
                          {6, 6, "0xf", "0x804", "0x0000000051ff804e", 652800, 2208000},
                          {7, 7, "0xf", "0x804", "0x0000000051ff804e", 806400, 2400000}};
    std::string cpuinfo;
    for (const Core& c : cores) {
        std::string related;
        for (int i = c.first; i <= c.last; ++i) related += std::to_string(i) + (i < c.last ? " " : "\n");
        auto put_freq = [&](const std::string& dir) {
            const std::pair<const char*, std::string> files[] = {
                {"cpuinfo_max_freq", std::to_string(c.max_khz)}, {"cpuinfo_min_freq", std::to_string(c.min_khz)},
                {"scaling_max_freq", std::to_string(c.max_khz)}, {"scaling_min_freq", std::to_string(c.min_khz)},
                {"scaling_cur_freq", std::to_string(c.max_khz)}};
            for (const auto& [f, v] : files)
                if (!write_mode(dir + "/" + f, v + "\n", 0644, error)) return false;
            return write_mode(dir + "/related_cpus", related, 0644, error) &&
                   write_mode(dir + "/affected_cpus", related, 0644, error);
        };
        for (int i = c.first; i <= c.last; ++i) {
            const std::string cpu = D + "/sys-cpu/cpu" + std::to_string(i);
            if (!mkdirs_or_fail(cpu + "/cpufreq", error) || !mkdirs_or_fail(cpu + "/regs/identification", error) ||
                !put_freq(cpu + "/cpufreq") ||
                !write_mode(cpu + "/regs/identification/midr_el1", std::string(c.midr) + "\n", 0644, error) ||
                !write_mode(cpu + "/regs/identification/revidr_el1", "0x0000000000000000\n", 0644, error))
                return false;
            // A policy per cluster, named for its first core; the phone's
            // policy dirs are shown under these names (tawcroot ranks them)
            const std::string policy = D + "/sys-cpu/cpufreq/policy" + std::to_string(c.first);
            if (i == c.first && (!mkdirs_or_fail(policy, error) || !put_freq(policy))) return false;
            cpuinfo += "processor\t: " + std::to_string(i) +
                       "\nBogoMIPS\t: 38.40\n"
                       "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc "
                       "dcpop asimddp\n"
                       "CPU implementer\t: 0x51\nCPU architecture: 8\nCPU variant\t: " + c.variant +
                       "\nCPU part\t: " + c.part + "\nCPU revision\t: 14\n\n";
        }
    }
    cpuinfo += "Hardware\t: Qualcomm Technologies, Inc LITO\n";
#if defined(__aarch64__)
    // AT_HWCAP of the Features above (fp .. asimddp), no AT_HWCAP2 ones
    if (!write_mode(D + "/hwcap", "0x119fff 0x0\n", 0644, error)) return false;
#endif
    return write_mode(D + "/cpuinfo", cpuinfo, 0644, error) &&
           // TQ3A.230901.001.C2's boot.img kernel ("Linux version ..." in its Image)
           write_mode(D + "/uname", "4.19.269-g8728c337137c-ab10161573\n#1 SMP PREEMPT Thu May 18 11:19:41 UTC 2023\n",
                      0644, error) &&
           write_mode(D + "/meminfo-kb", "7612308\n", 0644, error) &&
           // The GPU drivers' sockets (see the symlinks below) are no phone's;
           // apps open them but don't list them. /dev itself apps can't list.
           write_mode(D + "/unlisted", "/dev/socket/gralloc\n/dev/socket/kgsl-0\n", 0644, error) &&
           write_mode(D + "/unlistable", "/dev\n", 0644, error) &&
           write_mode(D + "/mounts", device_mounts(R), 0644, error) &&
           // 110.5 GiB: Android reports the device's storage as the power of two above, 128 GB
           write_mode(D + "/data-bytes", "118648471552\n", 0644, error);
}

// The Pixel 5's hardware features where Waydroid's differ: no camera
// without the camera HAL (declaring one that won't open is worse than none),
// Bluetooth LE. LineageOS's TERMINFO would be in every app's environment.
bool apply_device_features(const std::string& R, bool camera, std::string* error) {
    const std::string P = R + "/vendor/etc/permissions";
    for (const auto& [f, features] : kCameraFeatures)
        if (!camera) unlink((P + "/" + f).c_str());
    const std::pair<const char*, const char*> drop[] = {
        {"/vendor/etc/permissions/handheld_core_hardware.xml", "    <feature name=\"android.hardware.camera\" />\n"},
        {"/system/system_ext/etc/init/init.lineage-system_ext.rc", "    export TERMINFO /system_ext/etc/terminfo\n"},
    };
    for (const auto& [f, line] : drop) {
        const auto text = read_file(R + f);
        if (!text) continue;
        const size_t at = text->find(line);
        if (at != std::string::npos && !write_mode(R + f, text->substr(0, at) + text->substr(at + strlen(line)), 0644, error))
            return false;
    }
    return is_directory(P) &&
           write_mode(P + "/android.hardware.bluetooth_le.xml",
                      "<permissions>\n    <feature name=\"android.hardware.bluetooth_le\" />\n</permissions>\n", 0644, error);
}

// Waydroid's legacy HALs that hw_get_module finds by ro.board.platform,
// which the guest's identity sets to the Pixel 5's: named for ro.hardware,
// looked up first, instead
bool install_device_hals(const std::string& R, std::string* error) {
    for (const char* dir : {"/vendor/lib/hw/", "/vendor/lib64/hw/"}) {
        for (const char* module : {"audio.primary", "gatekeeper", "hwcomposer", "memtrack"}) {
            const std::string from = R + dir + module + ".waydroid.so", to = R + dir + module + ".redfin.so";
            if (is_regular_file(from) && rename(from.c_str(), to.c_str()) != 0)
                return fail(error, "could not rename " + from + ": " + strerror(errno));
        }
    }
    return true;
}

// LineageOS's and Waydroid's own apps and overlays, which a Pixel lacks
// and any app can list. Kept: the browser and camera apps' intents need,
// and the Lineage framework packages Settings and SystemUI depend on.
const char* const kForeignApps[] = {
    "/system/product/app/Backgrounds",         "/system/product/app/Etar",
    "/system/product/app/Recorder",            "/system/product/app/ApertureLensLauncher",
    "/system/product/priv-app/Eleven",         "/system/product/overlay/FontRubik",
    "/system/product/overlay/FontLato",        "/system/product/overlay/LineageBlackTheme",
    "/system/product/overlay/LineageNavigationBarNoHint",
    "/system/system_ext/app/Profiles",         "/system/system_ext/priv-app/WaydroidUpdater",
    "/system/system_ext/priv-app/Seedvault",
};

bool remove_foreign_apps(const std::string& R, std::string* error) {
    for (const char* app : kForeignApps) {
        if (!remove_if_exists(R + app, error)) return false;
    }
    return true;
}

// Files and directories that name Waydroid, LineageOS, the emulator or
// the engine: tawcroot's <store>/app-hidden (tawcroot/include/app_hidden.h),
// which apps see as absent. Kept visible: installed apps, overlays and
// APEXes. The marked libraries only services load; Lineage's framework
// resources apps inherit mapped from zygote (tawcroot leaves the mapping
// out of their /proc/self/maps).
bool marked_name(const std::string& name) {
    std::string n = name;
    for (char& c : n) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    // minigbm/gbm_mesa: Waydroid's gralloc 4, unused (gralloc.redfin is
    // gralloc 2); hidden, apps' libui finds no mapper 4 and takes gralloc 2's
    for (const char* m : {"waydroid", "lineage", "vine", "goldfish", "ranchu", "cuttlefish", "gfxstream", "anbox", "qemu",
                          "virtio", "reference-ril", "minigbm", "gbm_mesa"}) {
        if (n.find(m) != std::string::npos) return true;
    }
    return false;
}

void collect_hidden(const std::string& R, const std::string& dir, int depth, std::vector<std::string>* out) {
    DIR* d = opendir((R + dir).c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = dir + "/" + name;
        struct stat st {};
        if (lstat((R + path).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (name == "app" || name == "priv-app" || name == "overlay" || name == "apex") continue;
            if (marked_name(name)) out->push_back(path);
            else if (depth < 6) collect_hidden(R, path, depth + 1, out);
        } else if (marked_name(name)) {
            out->push_back(path);
        }
    }
    closedir(d);
}

std::string app_hidden_list(const std::string& R) {
    std::vector<std::string> paths;
    collect_hidden(R, "/system", 0, &paths);
    collect_hidden(R, "/vendor", 0, &paths);
    // The engine's own: tawcroot's port table and its runtime directory
    std::string out = "/.tawcroot-tcp-v1\n/run\n";
    for (const std::string& p : paths) {
        out += p + "\n";
        // $R/product and $R/system_ext are symlinks into system
        for (const char* part : {"/system/product/", "/system/system_ext/"}) {
            if (p.rfind(part, 0) == 0) out += p.substr(7) + "\n";
        }
    }
    return out;
}

// The guest's jars that scripts/rootless/pmpatch patches (Main.java says
// how), on the host's ART: services.jar, so that apps do not see LineageOS's
// packages, service-connectivity.jar, so that the network is a Wi-Fi
// network, and service-wifi.jar, so that WifiManager reports it. The jar as
// extracted is kept as <store>/original-<name> and patched from there, so a
// new patcher replaces the old one's work; <store>/patched-<name> records
// which jar (size and mtime) and patcher (a hash: libraries an APK installs
// all have the same mtime) the jar in the rootfs comes from.
// Without the patcher or ART, or on a jar it does not know, the guest boots
// it unpatched. A patched jar's odex no longer matches; ART runs the dex.
const char* const kPatchedJars[] = {
    "/system/framework/services.jar",
    "/system/apex/com.android.tethering/javalib/service-connectivity.jar",
    "/system/apex/com.android.wifi/javalib/service-wifi.jar",
};

std::string stat_mark(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 ? std::to_string(st.st_size) + " " + std::to_string(st.st_mtime) : "";
}

bool patch_guest_jar(const Layout& l, const std::string& lib_dir, const std::string& path, std::string* error) {
    const std::string patcher = lib_dir + "/libvine_pm_patch.so";
    const char* const vm = "/apex/com.android.art/bin/dalvikvm64";
    const std::string name = path.substr(path.rfind('/') + 1);
    const std::string jar = l.rootfs + path, out = jar + ".vine";
    const std::string stamp = l.store + "/patched-" + name, original = l.store + "/original-" + name;
    if (!is_regular_file(patcher) || access(vm, X_OK) != 0 || stat_mark(jar).empty()) return true;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : read_file(patcher).value_or("")) h = (h ^ c) * 0x100000001b3ull;
    const std::string patcher_hash = std::to_string(h);
    const auto mark = [&]() { return stat_mark(jar) + " " + patcher_hash + "\n"; };
    if (read_file(stamp).value_or("") == mark()) return true;
    // No stamp: the jar is as extracted. A stamp without the original is an
    // earlier engine's, which patched in place: the patcher takes its jar.
    if (!is_regular_file(original) && !is_regular_file(stamp) && !copy_mode(jar, original, 0644, error)) return false;
    const std::string from = is_regular_file(original) ? original : jar;
    unlink(out.c_str());
    const pid_t pid = fork();
    if (pid == 0) {
        execl(vm, vm, "-Xmx1g", "-cp", patcher.c_str(), "dev.vine.pmpatch.Main", from.c_str(), out.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid) return fail_errno(error, "run the jar patcher");
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code == 0 && (chmod(out.c_str(), 0644) != 0 || rename(out.c_str(), jar.c_str()) != 0))
        return fail_errno(error, "replace " + jar);
    unlink(out.c_str());
    // 2: already patched, or not a jar it knows; anything else is retried next boot
    if (code != 0 && code != 2) {
        fprintf(stderr, "patching %s failed (status %d); booting it unpatched\n", path.c_str(), code);
        return true;
    }
    return write_mode(stamp, mark(), 0644, error);
}

// Properties that name LineageOS or Waydroid, in vendor_default_prop:
// tawcroot denies apps that area, the guest's services keep reading them
const char kHiddenPropContexts[] =
    "waydroid.               u:object_r:vendor_default_prop:s0\n"
    "persist.waydroid.       u:object_r:vendor_default_prop:s0\n"
    "ro.lineage.             u:object_r:vendor_default_prop:s0\n"
    "sys.lineage_            u:object_r:vendor_default_prop:s0\n"
    "ro.modversion           u:object_r:vendor_default_prop:s0 exact string\n"
    "ro.lineagelegal.url     u:object_r:vendor_default_prop:s0 exact string\n";

} // namespace

Layout layout_for(const std::string& instance_path) {
    Layout l;
    l.instance = instance_path;
    l.work = instance_path + "/rootless";
    l.rootfs = l.work + "/rootfs";
    l.store = l.work + "/tawcroot";
    l.binderfs = l.work + "/binderfs";
    l.data = l.work + "/data";
    l.run = l.work + "/run";
    l.input = l.work + "/input";
    l.logs = l.work + "/logs";
    l.vinefb = instance_path + "/shared/vinefb";
    return l;
}

std::string props_64bit_only(const std::string& build_prop) {
    std::string out;
    size_t pos = 0;
    while (pos < build_prop.size()) {
        size_t nl = build_prop.find('\n', pos);
        const bool has_nl = nl != std::string::npos;
        std::string line = build_prop.substr(pos, has_nl ? nl - pos : std::string::npos);
        pos = has_nl ? nl + 1 : build_prop.size();
        const size_t eq = line.find('=');
        const std::string key = eq == std::string::npos ? "" : line.substr(0, eq);
        // ro.<partition>.product.cpu.abilist[32], partition in [a-z]*
        size_t p = 3;
        while (key.rfind("ro.", 0) == 0 && p < key.size() && key[p] >= 'a' && key[p] <= 'z') ++p;
        const std::string rest = key.rfind("ro.", 0) == 0 ? key.substr(p) : "";
        if (rest == ".product.cpu.abilist") line = key + "=arm64-v8a";
        else if (rest == ".product.cpu.abilist32") line = key + "=";
        else if (key == "ro.zygote") line = "ro.zygote=zygote64";
        out += line;
        if (has_nl) out += "\n";
    }
    return out;
}

DeviceIdentity guest_identity() {
    DeviceIdentity id;
    id.brand = "google";
    id.manufacturer = "Google";
    id.model = "Pixel 5";
    id.name = id.device = id.board = id.hardware = id.build_product = "redfin";
    id.build_id = "TQ3A.230901.001.C2";
    id.incremental = "10753682";
    id.fingerprint = "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys";
    id.description = "redfin-user 13 TQ3A.230901.001.C2 10753682 release-keys";
    id.flavor = "redfin-user";
    id.build_host = "abfarm-release-rbe-64-2004-0087";
    id.date = "Sat Sep  2 01:48:29 UTC 2023";
    id.date_utc = "1693619309";
    id.security_patch = "2023-09-01";
    id.soc_manufacturer = "QTI";
    id.soc_model = "SM7250";
    id.bootloader = kBootloader;
    id.baseband = kBaseband;
    id.revision = "MP1.0";  // Pixels' form, unverified on a device
    id.width = 1080;
    id.height = 2340;
    id.dpi = 440;
    id.refresh_hz = 90;
    id.props = {
        // Snapdragon 765G's; Waydroid's "waydroid" otherwise. Its HALs that
        // were found by it are renamed for ro.hardware (install_device_hals)
        {"ro.board.platform", "lito"},
        {"ro.product.product.brand", "google"},
        {"ro.product.product.device", "redfin"},
        {"ro.product.product.manufacturer", "Google"},
        {"ro.product.product.model", "Pixel 5"},
        {"ro.product.product.name", "redfin"},
        {"ro.product.build.date", "Sat Sep  2 01:48:29 UTC 2023"},
        {"ro.product.build.date.utc", "1693619309"},
        {"ro.product.build.fingerprint", "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys"},
        {"ro.product.build.id", "TQ3A.230901.001.C2"},
        {"ro.product.build.tags", "release-keys"},
        {"ro.product.build.type", "user"},
        {"ro.product.build.version.incremental", "10753682"},
        {"ro.product.vendor.brand", "google"},
        {"ro.product.vendor.device", "redfin"},
        {"ro.product.vendor.manufacturer", "Google"},
        {"ro.product.vendor.model", "Pixel 5"},
        {"ro.product.vendor.name", "redfin"},
        {"ro.vendor.build.date", "Sat Sep  2 01:48:29 UTC 2023"},
        {"ro.vendor.build.date.utc", "1693619309"},
        {"ro.vendor.build.fingerprint", "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys"},
        {"ro.vendor.build.id", "TQ3A.230901.001.C2"},
        {"ro.vendor.build.tags", "release-keys"},
        {"ro.vendor.build.type", "user"},
        {"ro.vendor.build.version.incremental", "10753682"},
        {"ro.vendor.build.security_patch", "2023-09-01"},
        {"ro.bootimage.build.date", "Sat Sep  2 01:48:29 UTC 2023"},
        {"ro.bootimage.build.date.utc", "1693619309"},
        {"ro.bootimage.build.fingerprint", "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys"},
        {"ro.product.odm.brand", "google"},
        {"ro.product.odm.device", "redfin"},
        {"ro.product.odm.manufacturer", "Google"},
        {"ro.product.odm.model", "Pixel 5"},
        {"ro.product.odm.name", "redfin"},
        {"ro.odm.build.date", "Sat Sep  2 01:48:29 UTC 2023"},
        {"ro.odm.build.date.utc", "1693619309"},
        {"ro.odm.build.fingerprint", "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys"},
        {"ro.odm.build.id", "TQ3A.230901.001.C2"},
        {"ro.odm.build.tags", "release-keys"},
        {"ro.odm.build.type", "user"},
        {"ro.odm.build.version.incremental", "10753682"},
        {"ro.product.system.brand", "google"},
        {"ro.product.system.device", "redfin"},
        {"ro.product.system.manufacturer", "Google"},
        {"ro.product.system.model", "Pixel 5"},
        {"ro.product.system.name", "redfin"},
        // Launched on Android 11; Waydroid's image says 28
        {"ro.product.first_api_level", "30"},
        {"ro.board.first_api_level", "30"},
        {"ro.system.build.date", "Sat Sep  2 01:48:29 UTC 2023"},
        {"ro.system.build.date.utc", "1693619309"},
        {"ro.system.build.fingerprint", "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys"},
        {"ro.system.build.id", "TQ3A.230901.001.C2"},
        {"ro.system.build.tags", "release-keys"},
        {"ro.system.build.type", "user"},
        {"ro.system.build.version.incremental", "10753682"},
        {"ro.build.id", "TQ3A.230901.001.C2"},
        {"ro.build.display.id", "TQ3A.230901.001.C2"},
        {"ro.build.version.incremental", "10753682"},
        {"ro.build.version.security_patch", "2023-09-01"},
        {"ro.build.date", "Sat Sep  2 01:48:29 UTC 2023"},
        {"ro.build.date.utc", "1693619309"},
        {"ro.build.type", "user"},
        {"ro.build.user", "android-build"},
        {"ro.build.host", "abfarm-release-rbe-64-2004-0087"},
        {"ro.build.tags", "release-keys"},
        {"ro.build.flavor", "redfin-user"},
        {"ro.build.product", "redfin"},
        {"ro.build.description", "redfin-user 13 TQ3A.230901.001.C2 10753682 release-keys"},
        {"ro.build.fingerprint", "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys"},
        {"ro.product.brand", "google"},
        {"ro.product.device", "redfin"},
        {"ro.product.manufacturer", "Google"},
        {"ro.product.model", "Pixel 5"},
        {"ro.product.name", "redfin"},
        {"ro.product.system_ext.brand", "google"},
        {"ro.product.system_ext.device", "redfin"},
        {"ro.product.system_ext.manufacturer", "Google"},
        {"ro.product.system_ext.model", "Pixel 5"},
        {"ro.product.system_ext.name", "redfin"},
        {"ro.system_ext.build.date", "Sat Sep  2 01:48:29 UTC 2023"},
        {"ro.system_ext.build.date.utc", "1693619309"},
        {"ro.system_ext.build.fingerprint", "google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys"},
        {"ro.system_ext.build.id", "TQ3A.230901.001.C2"},
        {"ro.system_ext.build.tags", "release-keys"},
        {"ro.system_ext.build.type", "user"},
        {"ro.system_ext.build.version.incremental", "10753682"},
    };
    return id;
}


namespace {

// ro.product.<field> or ro.product.<partition>.<field>: the field, else ""
std::string product_field(const std::string& key) {
    if (key.rfind("ro.product.", 0) != 0) return "";
    std::string rest = key.substr(11);
    const size_t dot = rest.find('.');
    if (dot != std::string::npos) rest = rest.substr(dot + 1);
    return rest.find('.') == std::string::npos ? rest : "";
}

// ro.build.<field> or ro.<partition>.build.<field> (ro.product.build.* too):
// the field, possibly dotted (version.incremental), else ""
std::string build_field(const std::string& key) {
    if (key.rfind("ro.build.", 0) == 0) return key.substr(9);
    if (key.rfind("ro.", 0) != 0) return "";
    const size_t dot = key.find('.', 3);
    if (dot == std::string::npos || key.compare(dot, 7, ".build.") != 0) return "";
    return key.substr(dot + 7);
}

} // namespace

std::string props_device(const std::string& build_prop, const DeviceIdentity& id) {
    if (id.empty()) return build_prop;
    std::string out;
    size_t pos = 0;
    while (pos < build_prop.size()) {
        size_t nl = build_prop.find('\n', pos);
        const bool has_nl = nl != std::string::npos;
        std::string line = build_prop.substr(pos, has_nl ? nl - pos : std::string::npos);
        pos = has_nl ? nl + 1 : build_prop.size();
        const size_t eq = line.find('=');
        if (eq != std::string::npos && !line.empty() && line[0] != '#') {
            const std::string key = line.substr(0, eq);
            const std::string product = product_field(key), build = build_field(key);
            const std::string* set = nullptr;
            static const std::string zero = "0", one = "1", none = "none", user = "user", release_keys = "release-keys",
                                     android_build = "android-build";
            // The device's own line first, else one made from its fields
            for (const auto& [k, v] : id.props) {
                if (k == key) set = &v;
            }
            if (set) {
            }
            // A phone's: risk SDKs (汽水音乐's) block on debuggable or adb-on
            // builds. The engine's exec channel needs neither.
            else if (key == "ro.debuggable") set = &zero;
            else if (key == "ro.adb.secure") set = &one;
            else if (key == "persist.sys.usb.config") set = &none;
            else if (product == "brand") set = &id.brand;
            else if (product == "manufacturer") set = &id.manufacturer;
            else if (product == "model") set = &id.model;
            else if (product == "name") set = &id.name;
            else if (product == "device") set = &id.device;
            else if (product == "board") set = &id.board;
            // user, as risk SDKs (汽水音乐's) block userdebug. Costs a boot-time
            // "There's an internal problem with your device" dialog: on user,
            // Build.isBuildConsistent() runs the VINTF check, which a guest
            // cannot pass (no kernel sepolicy version or config, a kernel
            // newer than the matrices know). Nothing else reads that result.
            else if (build == "type") set = &user;
            else if (build == "tags") set = &release_keys;
            else if (build == "user") set = &android_build;
            else if (build == "id" || build == "display.id") set = &id.build_id;
            else if (build == "version.incremental") set = &id.incremental;
            else if (build == "fingerprint") set = &id.fingerprint;
            else if (build == "description") set = &id.description;
            else if (build == "flavor") set = &id.flavor;
            else if (build == "host") set = &id.build_host;
            else if (build == "product") set = &id.build_product;
            else if (build == "date") set = &id.date;
            else if (build == "date.utc") set = &id.date_utc;
            else if (build == "version.security_patch" || build == "security_patch") set = &id.security_patch;
            else if (key == "ro.soc.manufacturer") set = &id.soc_manufacturer;
            else if (key == "ro.soc.model") set = &id.soc_model;
            if (set && !set->empty()) line = key + "=" + *set;
        }
        out += line;
        if (has_nl) out += "\n";
    }
    return out;
}

DeviceIdentity rootfs_identity(const std::string& rootfs) {
    const auto system_prop = read_file(rootfs + "/system/build.prop");
    if (!system_prop || system_prop->find("\nro.build.version.release=13\n") == std::string::npos) return {};
    return guest_identity();
}

std::string guest_cmdline(const DeviceIdentity& id, const std::string& serial) {
    std::string out;
    if (!id.hardware.empty()) out += "androidboot.hardware=" + id.hardware + " ";
    if (!id.bootloader.empty()) out += "androidboot.bootloader=" + id.bootloader + " ";
    if (!id.baseband.empty()) out += "androidboot.baseband=" + id.baseband + " ";
    if (!id.revision.empty()) out += "androidboot.revision=" + id.revision + " androidboot.hardware.revision=" + id.revision + " ";
    if (!serial.empty()) out += "androidboot.serialno=" + serial + " ";
    return out + "androidboot.verifiedbootstate=green androidboot.flash.locked=1 "
                 "androidboot.vbmeta.device_state=locked androidboot.veritymode=enforcing\n";
}

DeviceIds device_ids(const std::string& seed) {
    uint64_t s = 0xcbf29ce484222325ull;
    for (unsigned char c : seed) s = (s ^ c) * 0x100000001b3ull;
    auto next = [&s] {  // splitmix64
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    };
    DeviceIds ids;
    // Pixels' form: 0, a year letter, 2 digits, the factory's 1FDD400, a
    // digit, 2 characters
    static const char alnum[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    ids.serial = std::string("0") + "9AB"[next() % 3] + char('0' + next() % 10) + char('0' + next() % 10) + "1FDD400" +
                 char('0' + next() % 10) + alnum[next() % 34] + alnum[next() % 34];
    uint64_t m = next();
    char mac[18];
    snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", (unsigned)((m & 0xfc) | 0x02), (unsigned)(m >> 8 & 0xff),
             (unsigned)(m >> 16 & 0xff), (unsigned)(m >> 24 & 0xff), (unsigned)(m >> 32 & 0xff), (unsigned)(m >> 40 & 0xff));
    ids.mac = mac;
    // Google's 3C:28:6D, one of the OUIs Pixels' Bluetooth addresses carry
    m = next();
    snprintf(mac, sizeof mac, "3C:28:6D:%02X:%02X:%02X", (unsigned)(m & 0xff), (unsigned)(m >> 8 & 0xff),
             (unsigned)(m >> 16 & 0xff));
    ids.bt_mac = mac;
    return ids;
}

std::string device_seed(const Layout& l) {
    const std::string path = l.work + "/device.seed";
    std::string seed = read_file(path).value_or("");
    while (!seed.empty() && seed.back() == '\n') seed.pop_back();
    if (seed.size() == 32 && seed.find_first_not_of("0123456789abcdef") == std::string::npos) return seed;
    seed = random_hex(16);
    if (seed.empty() || !write_file_atomic(path, seed + "\n")) return "";
    return seed;
}

std::vector<std::pair<std::string, std::string>> parse_environ_rc(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> env;
    for (const auto& raw : split_lines(text)) {
        size_t a = raw.find_first_not_of(" \t");
        if (a == std::string::npos || raw.compare(a, 7, "export ") != 0) continue;
        a = raw.find_first_not_of(" \t", a + 7);
        if (a == std::string::npos) continue;
        const size_t b = raw.find_first_of(" \t", a);
        if (b == std::string::npos) continue;
        const size_t c = raw.find_first_not_of(" \t", b);
        if (c == std::string::npos) continue;
        const size_t d = raw.find_last_not_of(" \t");
        env.emplace_back(raw.substr(a, b - a), raw.substr(c, d + 1 - c));
    }
    return env;
}

bool extract_rootfs(const Layout& l, std::string* error) {
    const std::string stamp_path = l.work + "/rootfs.stamp";
    const std::string stamp = image_stamp(l);
    if (is_directory(l.rootfs) && read_file(stamp_path).value_or("") == stamp) return true;
    if (!is_regular_file(l.instance + "/system.img")) return fail(error, "this instance has no system.img");

    unlink(stamp_path.c_str());
    unlink((l.work + "/device.seed").c_str());
    if (!remove_if_exists(l.rootfs, error) || !remove_if_exists(l.store, error) || !remove_if_exists(l.data, error)) return false;
    if (!mkdirs_or_fail(l.rootfs, error) || !mkdirs_or_fail(l.store, error)) return false;
    const std::string owners = l.store + "/owners";
    char err[512];
    if (vine_ext4_extract((l.instance + "/system.img").c_str(), l.rootfs.c_str(), owners.c_str(), err, sizeof err) != 0) {
        return fail(error, std::string("system.img: ") + err);
    }
    if (is_regular_file(l.instance + "/vendor.img")) {
        if (!mkdirs_or_fail(l.rootfs + "/vendor", error)) return false;
        if (vine_ext4_extract((l.instance + "/vendor.img").c_str(), (l.rootfs + "/vendor").c_str(), owners.c_str(), err, sizeof err) != 0) {
            return fail(error, std::string("vendor.img: ") + err);
        }
    }
    if (!is_regular_file(l.rootfs + "/system/bin/init")) return fail(error, "system.img is not a system-as-root image (no /system/bin/init)");
    return write_file_atomic(stamp_path, stamp) || fail(error, "could not write " + stamp_path);
}

bool prepare_rootfs(const Layout& l, const PrepOptions& o, std::string* error) {
    const std::string& R = l.rootfs;

    // Flattened APEXes, where /apex/<name> is expected
    if (DIR* d = opendir((R + "/system/apex").c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            struct stat st{};
            const std::string link = R + "/apex/" + n;
            if (lstat(link.c_str(), &st) != 0 && symlink(("/system/apex/" + n).c_str(), link.c_str()) != 0) {
                closedir(d);
                return fail_errno(error, "symlink " + link);
            }
        }
        closedir(d);
    }

    if (!remove_if_exists(l.binderfs, error)) return false;
    for (const auto& dir : {l.binderfs, l.data, l.run, l.logs, l.instance + "/shared"}) {
        if (!mkdirs_or_fail(dir, error)) return false;
    }

    // The previous boot's runtime state: the guest's /dev, tawcroot's
    // per-session store files (the ownership table stays), the boot signal
    for (const char* n : {"null", "zero", "random", "urandom", "kmsg"}) {
        const std::string p = R + "/dev/" + n;
        if (is_regular_file(p)) unlink(p.c_str());
    }
    for (const char* n : {"__properties__", "cgroup_info", ".booting", "socket", "pts", "input"}) {
        if (!remove_if_exists(R + "/dev/" + n, error)) return false;
    }
    for (const char* n : {"mounts", "mounts.event", "mnt", "cgroup", "cgroup.registry", "uts", "ids"}) {
        if (!remove_if_exists(l.store + "/" + n, error)) return false;
    }
    unlink((l.run + "/boot_completed").c_str());
    if (!write_file(R + "/dev/kmsg", "")) return fail(error, "could not create /dev/kmsg");

    // A host without AArch32 can't run the ROM's 32-bit ABIs: present it
    // as 64-bit only, like the host
    const std::string hal_service = o.lib_dir + "/libvine_hal_service.so";
    // Only for images whose audio HAL service is 32-bit (64_32 builds;
    // waydroid_arm64_only ships a 64-bit one)
    const bool own_audio_hal = !o.host_has_32bit && is_regular_file(hal_service) &&
                               is_elf32(R + "/vendor/bin/hw/android.hardware.audio.service");
    // A Pixel 5's identity instead of Waydroid's (apps reading Build.* or
    // the fingerprint see its Android 13 release build), for an Android 13
    // image only: on another release the identity would contradict it
    const DeviceIdentity id = rootfs_identity(R);
    if (!id.empty()) {
        std::vector<std::pair<std::string, std::string>> files;
        for (const char* f : {"/system/build.prop", "/system/system_ext/etc/build.prop", "/system/product/etc/build.prop",
                              "/vendor/build.prop", "/vendor/odm/etc/build.prop", "/vendor/vendor_dlkm/etc/build.prop",
                              "/odm/etc/build.prop"}) {
            if (const auto text = read_file(R + f)) files.emplace_back(f, *text);
        }
        for (auto& [f, text] : files) {
            const std::string edited = props_device(text, id);
            if (edited != text && !write_mode(R + f, edited, 0644, error)) return false;
            text = "\n" + edited;
        }
        // What the device sets that the image's partitions don't (Waydroid
        // sets no ro.product.board or ro.soc.*), into /system/build.prop
        std::vector<std::pair<std::string, std::string>> wanted = id.props;
        wanted.insert(wanted.end(), {{"ro.product.board", id.board},
                                     {"ro.soc.manufacturer", id.soc_manufacturer},
                                     {"ro.soc.model", id.soc_model}});
        std::string add;
        for (const auto& [k, v] : wanted) {
            bool present = false;
            for (const auto& [f, text] : files) present = present || text.find("\n" + k + "=") != std::string::npos;
            if (!present && add.find("\n" + k + "=") == std::string::npos) add += "\n" + k + "=" + v;
        }
        if (!add.empty() && !files.empty() && files[0].first == "/system/build.prop") {
            std::string edited = files[0].second.substr(1);
            if (!edited.empty() && edited.back() != '\n') edited += "\n";
            if (!write_mode(R + files[0].first, edited + add.substr(1) + "\n", 0644, error)) return false;
        }
    }
    const std::string seed = device_seed(l);
    if (seed.empty()) return fail(error, "could not write " + l.work + "/device.seed");
    const DeviceIds ids = device_ids(seed);
    // tawcroot serves it as /proc/cmdline (ro.hardware, serial, verified
    // boot); apps are denied it, as on a phone
    if (!write_mode(l.store + "/cmdline", guest_cmdline(id, ids.serial), 0644, error)) return false;
    // tawcroot's wlan0 address, and the guest's boot_id: new each boot as on
    // a phone, not the host's that every instance would share
    const std::string boot_id = random_boot_id();
    if (boot_id.empty()) return fail(error, "/dev/urandom unreadable");
    if (!write_mode(l.store + "/mac", ids.mac + "\n", 0644, error) || !write_mode(l.store + "/boot_id", boot_id, 0644, error))
        return false;
    if (!o.host_has_32bit) {
        for (const char* f : {"/system/build.prop", "/vendor/build.prop", "/vendor/odm/etc/build.prop"}) {
            const auto text = read_file(R + f);
            if (!text) continue;
            const std::string edited = props_64bit_only(*text);
            if (edited != *text && !write_mode(R + f, edited, 0644, error)) return false;
        }
        const std::string z = R + "/system/etc/init/hw";
        if (!is_regular_file(z + "/init.zygote64.rc")) {
            const auto z6432 = read_file(z + "/init.zygote64_32.rc");
            if (!z6432) return fail(error, "the image has neither init.zygote64.rc nor init.zygote64_32.rc");
            if (!write_mode(z + "/init.zygote64.rc", zygote64_rc(*z6432), 0644, error)) return false;
        }
        if (own_audio_hal && !copy_mode(hal_service, R + "/vendor/bin/hw/vine_hal_service", 0755, error)) return false;
        if (!drop_32bit_services(R, own_audio_hal ? "android.hardware.audio.service" : "", error)) return false;
    }
    // waydroid_arm64_only declares the omx HAL but has no service for it:
    // mediaserver then waits for IOmx forever, and with it everything that
    // lists codecs (MediaProvider never comes up, ringtones hang Settings)
    if (!is_regular_file(R + "/vendor/bin/hw/android.hardware.media.omx@1.0-service") &&
        !drop_vendor_hals(R, {"android.hardware.media.omx"}, error))
        return false;
    if (!copy_mode(o.lib_dir + "/libvine_lmkd.so", R + "/system/bin/vine_lmkd", 0755, error)) return false;
    // vold mounts emulated storage only through FUSE: the shim fakes that
    // part over the binds vine-rootless.rc makes
    const std::string vold_rc = R + "/system/etc/init/vold.rc";
    if (const auto text = read_file(vold_rc)) {
        if (!mkdirs_or_fail(R + "/system/lib64", error) ||
            !copy_mode(o.lib_dir + "/libvine_vold_shim.so", R + "/system/lib64/libvine_vold_shim.so", 0644, error) ||
            (text->find("libvine_vold_shim") == std::string::npos &&
             !write_mode(vold_rc, with_service_option(*text, "vold", "setenv LD_PRELOAD /system/lib64/libvine_vold_shim.so"), 0644, error)))
            return false;
    }
    // The padded gralloc takes gralloc.default's place (the hwcomposer and
    // Mesa tell gralloc kinds apart by ro.hardware.gralloc) and loads it
    // under another name
    const std::string hw = R + "/vendor/lib64/hw";
    if (is_regular_file(hw + "/gralloc.default.so") && !is_regular_file(hw + "/gralloc.default_real.so") &&
        rename((hw + "/gralloc.default.so").c_str(), (hw + "/gralloc.default_real.so").c_str()) != 0)
        return fail_errno(error, "rename gralloc.default.so");
    if (is_regular_file(hw + "/gralloc.default_real.so") &&
        !copy_mode(o.lib_dir + "/libvine_gralloc.so", hw + "/gralloc.default.so", 0644, error))
        return false;
    // $R/product is an absolute symlink, its files are in system/product
    const std::string product_init = R + "/system/product/etc/init";
    if (!mkdirs_or_fail(product_init, error) || !write_mode(product_init + "/vine-rootless.rc", rootless_rc(own_audio_hal, o.gpu, o.ram_mb), 0644, error)) return false;
    const std::string product_vine = R + "/system/product/etc/vine";
    if (!mkdirs_or_fail(product_vine, error) || !write_mode(product_vine + "/reap_orphans.sh", kReapOrphans, 0755, error) ||
        !write_mode(product_vine + "/check_display.sh", kCheckDisplay, 0755, error) ||
        !write_mode(product_vine + "/captive_portal_urls.sh", kCaptivePortalUrls, 0755, error) ||
        !write_mode(product_vine + "/device_settings.sh", device_settings_sh(ids.bt_mac), 0755, error))
        return false;
    bool radio = false;
    if (!install_radio_hal(R, o.lib_dir, &radio, error)) return false;
    // see kEglDriverProp: in place, and once (the patched file no longer has the name)
    bool egl_renamed = false;
    if (!id.empty()) {
        const std::string egl = R + "/system/lib64/libEGL.so";
        if (const auto so = read_file(egl)) {
            const std::pair<const char*, const char*> kEglStrings[] = {{"ro.hardware.egl", kEglDriverProp}};
            const std::string patched = replace_cstrings(*so, kEglStrings);
            if (patched != *so && !write_mode(egl, patched, 0644, error)) return false;
            egl_renamed = patched.find(std::string(1, '\0') + kEglDriverProp + '\0') != std::string::npos;
        }
    }
    // Mobile data off by default: vine_modem fails data calls, and the
    // RIL sets its data interface down when one ends, which may be wlan0
    if (!write_mode(R + "/vendor/waydroid.prop",
                    waydroid_prop(id.dpi ? id.dpi : o.dpi, o.gpu, egl_renamed) + (radio ? "ro.com.android.mobiledata=false\n" : ""), 0644, error))
        return false;
    // gralloc reads ro.vendor.display.gpu in every app, and tawcroot denies
    // apps the vendor_default_prop area that "ro.vendor." falls in
    const std::string vendor_prop_contexts = R + "/vendor/etc/selinux/vendor_property_contexts";
    if (const auto text = read_file(vendor_prop_contexts)) {
        std::string t = *text;
        if (!t.empty() && t.back() != '\n') t += "\n";
        if (t.find("\nro.vendor.display.gpu ") == std::string::npos)
            t += "ro.vendor.display.gpu u:object_r:exported_default_prop:s0 exact bool\n";
        if (t.find("\nwaydroid. ") == std::string::npos) t += kHiddenPropContexts;
        if (t != *text && !write_mode(vendor_prop_contexts, t, 0644, error)) return false;
    }
    // Earlier names, and the GPU's files when it is off
    for (const char* old : {"/vulkan.gfxstream.so", "/gralloc.vine.so"}) unlink((hw + old).c_str());
    if (!o.gpu) {
        unlink((hw + "/vulkan.adreno.so").c_str());
        unlink((R + "/vendor/lib64/libadreno_utils.so").c_str());
        unlink((hw + "/gralloc.redfin.so").c_str());
    }
    // gfxstream's driver behind vulkan_hal_shim.c, which names the Pixel 5's GPU
    const auto vk = o.gpu ? read_file(o.lib_dir + "/libvine_vk_gfxstream.so") : std::nullopt;
    if (o.gpu && (!vk || !write_mode(R + "/vendor/lib64/libadreno_utils.so", replace_cstrings(*vk, kVulkanStrings), 0644,
                                     error) ||
                  !copy_mode(o.lib_dir + "/libvine_vulkan_hal.so", hw + "/vulkan.adreno.so", 0644, error) ||
                  !copy_mode(o.lib_dir + "/libvine_gralloc.so", hw + "/gralloc.redfin.so", 0644, error)))
        return false;
    if (!install_sensors_hal(R, o.lib_dir, error)) return false;
    bool camera = false;
    if (!install_camera_hal(R, o.lib_dir, &camera, error)) return false;
    if (!install_gles_shim(R, o.lib_dir, error)) return false;
    std::string extra_hidden;
    if (o.gpu) {
        for (const char* f : kAngleHidden) extra_hidden += std::string(f) + "\n";
    }
    if (camera)
        for (const char* f : kCameraHidden) extra_hidden += std::string(f) + "\n";
    if (!id.empty() && !apply_device_features(R, camera, error)) return false;
    if (!patch_health_hal(R, error)) return false;
    if (id.empty() ? !remove_if_exists(l.store + "/device", error) : !write_device_view(R, l.store, error)) return false;
    // The guest's /sys/class/power_supply, which VineVMManager fills from
    // the phone's battery
    if (!mkdirs_or_fail(l.work + "/power_supply", error) || chmod((l.work + "/power_supply").c_str(), 0755) != 0)
        return fail_errno(error, "chmod power_supply");
    if (!install_device_hals(R, error) || !remove_foreign_apps(R, error) ||
        !write_mode(l.store + "/app-hidden", app_hidden_list(R) + extra_hidden, 0644, error) ||
        !std::all_of(std::begin(kPatchedJars), std::end(kPatchedJars),
                     [&](const char* jar) { return patch_guest_jar(l, o.lib_dir, jar, error); }))
        return false;
    // scripts/radio/build-overlay.sh: EthernetTracker takes tawcroot's wlan0
    // and reports it as Wi-Fi, which is how a phone without a SIM is online.
    // Without it wlan0 matches no interface regex and the guest is offline.
    // Named as the build system names a vendor RRO: apps can read the path.
    // VineTelephony.apk is the telephony overlay's name before.
    unlink((R + "/vendor/overlay/VineTelephony.apk").c_str());
    if (is_regular_file(o.lib_dir + "/libvine_wifinet_overlay.so") &&
        (!mkdirs_or_fail(R + "/vendor/overlay", error) ||
         !copy_mode(o.lib_dir + "/libvine_wifinet_overlay.so",
                    R + "/vendor/overlay/ServiceConnectivityResources__auto_generated_rro_vendor.apk", 0644, error)))
        return false;

    // A first boot has no app ops recorded, and LineageOS 20's SystemUI
    // then crashes on getPackagesForOps() returning null. Seed one that
    // persists: SYSTEM_ALERT_WINDOW allowed for the system package.
    const std::string appops = l.data + "/system/appops.xml";
    if (!path_exists(appops)) {
        if (!mkdirs_or_fail(l.data + "/system", error)) return false;
        if (!write_file(appops,
                        "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<app-ops v=\"1\">\n<pkg n=\"android\">\n"
                        "<uid n=\"1000\">\n<op n=\"24\" m=\"0\" />\n</uid>\n</pkg>\n</app-ops>\n")) {
            return fail(error, "could not write " + appops);
        }
    }

    // Read from the tethering apex's dir, migrated from the legacy one
    for (const char* dir : {"/misc/ethernet", "/misc/apexdata/com.android.tethering/misc/ethernet"}) {
        const std::string d = l.data + dir;
        if (!mkdirs_or_fail(d, error) || !write_mode(d + "/ipconfig.txt", ethernet_ipconfig(), 0644, error)) return false;
    }
    // Where 192.168.31.1 points until the app's next update; no file falls back to 8.8.8.8
    std::string dns;
    for (const std::string& s : o.dns) dns += s + "\n";
    if (!dns.empty() && !write_mode(l.store + "/dns", dns, 0644, error)) return false;

    // libbinder access()es the driver before opening it
    for (const char* b : {"binder", "hwbinder", "vndbinder"}) {
        if (!symlink_over(std::string("binderfs/") + b, R + "/dev/" + b, error)) return false;
    }
    // What first-stage init would have created; /dev/input empty for the
    // hwcomposer's FIFOs
    for (const char* d : {"/dev/socket", "/dev/pts", "/dev/input", "/mnt/vendor", "/mnt/product", "/run/user/1000"}) {
        if (!mkdirs_or_fail(R + d, error)) return false;
    }
    // What the Pixel 5's vendor init makes there (its vendor.img's
    // etc/init: always-running services' sockets and the dirs it mkdirs),
    // as apps can list /dev/socket; nothing serves them here
    if (!id.empty()) {
        const std::string S = R + "/dev/socket";
        for (const char* d : {"/qmux_audio", "/qmux_bluetooth", "/qmux_gps"}) rmdir((S + d).c_str());  // earlier guesses
        for (const char* d : {"/qmux_radio", "/location/mq", "/location/xtra", "/wifihal", "/netmgr", "/port-bridge"})
            if (!mkdirs_or_fail(S + d, error)) return false;
        for (const char* f : {"/wpa_wlan0", "/chre", "/ims_qmid", "/ims_datad", "/pps", "/msm_irqbalance", "/ssgtzd",
                              "/qwes_ipc"})
            mknod((S + f).c_str(), S_IFSOCK | 0660, 0);  // best effort: only their names show
    }
    // vine_kumquat's sockets where apps' GPU drivers find them (/run, the
    // engine's, is hidden from apps): gfxstream-vk's default path
    // (kVulkanStrings) and gralloc_gpu.c's
    if (!symlink_over("/run/user/1000/kumquat-gpu-0", R + "/dev/socket/kgsl-0", error) ||
        !symlink_over("/run/user/1000/gralloc-0", R + "/dev/socket/gralloc", error))
        return false;
    return mkdirs_or_fail(l.run + "/pulse", error);
}

} // namespace vine::rootless
