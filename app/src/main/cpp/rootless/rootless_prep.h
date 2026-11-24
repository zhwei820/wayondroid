#pragma once

#include <string>
#include <utility>
#include <vector>

// Rootfs preparation for a rootless (Waydroid) guest: what Waydroid's
// host and first-stage init would have done, see docs/rootless-boot.md.
namespace vine::rootless {

// Where one instance keeps its rootless state, all under the instance dir
struct Layout {
    std::string instance;   // <instance>: system.img, vendor.img, shared/
    std::string work;       // <instance>/rootless
    std::string rootfs;     // work/rootfs, the extracted images
    std::string store;      // work/tawcroot, tawcroot's store (<rootfs>/../tawcroot)
    std::string binderfs;   // binderd's directory, /dev/binderfs in the guest
    std::string data;       // the guest's /data
    std::string run;        // /run/user/1000: wayland-0, pulse/native, boot_completed
    std::string input;      // vine_wl's input socket
    std::string logs;       // one log per process
    std::string vinefb;     // <instance>/shared/vinefb, what the app displays
};

Layout layout_for(const std::string& instance_path);

struct PrepOptions {
    std::string lib_dir;    // where libvine_lmkd.so and libvine_hal_service.so are
    int dpi = 320;
    bool host_has_32bit = false;
    bool gpu = false;       // render through vine_kumquat (gfxstream)
    int ram_mb = 0;         // vine_lmkd's budget for the guest's apps, 0: none
    std::vector<std::string> dns; // the phone's DNS servers, written to <store>/dns for tawcroot
};

// Extracts system.img (and vendor.img into /vendor) unless the rootfs
// already holds these images. A fresh extraction also starts /data and
// the ownership table over: the old table's inode keys mean nothing for
// the new files.
bool extract_rootfs(const Layout& l, std::string* error);

// Readies an extracted rootfs for a boot. Run before every boot: it also
// clears the previous boot's runtime state.
bool prepare_rootfs(const Layout& l, const PrepOptions& o, std::string* error);

// `ro.<partition>.product.cpu.abilist*` set to 64-bit only and
// `ro.zygote=zygote64`; other lines unchanged.
std::string props_64bit_only(const std::string& build_prop);

// The device the guest presents instead of Waydroid
struct DeviceIdentity {
    std::string brand, manufacturer, model, name, device, board, hardware;
    std::string build_id, incremental, fingerprint, description, flavor, build_host, build_product;
    std::string date, date_utc, security_patch, soc_manufacturer, soc_model;
    // The device's own build.prop lines, exact; they win over the fields
    std::vector<std::pair<std::string, std::string>> props;
    bool empty() const { return brand.empty() || model.empty() || device.empty(); }
};

// A Google Pixel 5 on its Android 13 release: a device that ran the
// guest's Android version, unlike the phone. A Snapdragon, as the phone
// is, so the GPU's vendor agrees. Values from the factory image's
// build.props (redfin TQ3A.230901.001.C2).
DeviceIdentity guest_identity();

// A build.prop presenting `id` as a release build: every partition's
// product, build id, fingerprint, dates and patch level, release-keys
// tags, user type, not debuggable and adb off, keeping the image's
// Android release. Other lines unchanged; running it again changes
// nothing.
std::string props_device(const std::string& build_prop, const DeviceIdentity& id);

// /proc/cmdline as the guest's init reads it (through tawcroot, from
// <store>/cmdline): ro.hardware, ro.serialno (when given) and a locked,
// verified boot
std::string guest_cmdline(const DeviceIdentity& id, const std::string& serial = "");

// The identifiers that tell one phone from another, made from
// <work>/device.seed: a reset (a fresh extraction) draws a new seed, so
// the guest is a new device to apps; restarts keep it.
struct DeviceIds {
    std::string serial; // ro.serialno, a Pixel's 14 characters
    std::string mac;    // the fake wlan0's, aa:bb:cc:dd:ee:ff, locally administered as Android's randomized ones are
};
DeviceIds device_ids(const std::string& seed);

// <work>/device.seed, drawn if missing; "" if it can't be written
std::string device_seed(const Layout& l);

// `export NAME VALUE` lines of an init.environ.rc
std::vector<std::pair<std::string, std::string>> parse_environ_rc(const std::string& text);

} // namespace vine::rootless
