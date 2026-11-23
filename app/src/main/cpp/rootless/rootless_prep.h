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

// The phone's identity, which the guest presents instead of Waydroid's
struct DeviceIdentity {
    std::string brand, manufacturer, model, name, device, board, incremental, hardware;
    bool empty() const { return brand.empty() || model.empty() || device.empty(); }
};

// The phone's own ro.product.* and friends; empty off Android (host tests)
DeviceIdentity host_identity();

// A build.prop presenting `id` as a release build: every partition's
// ro.product.<part>.{brand,manufacturer,model,name,device}, release-keys
// tags, user type, user/release-keys fingerprints, not
// debuggable and adb off,
// keeping the image's Android release and build id. Other lines unchanged;
// running it again changes nothing.
std::string props_device(const std::string& build_prop, const DeviceIdentity& id);

// /proc/cmdline as the guest's init reads it (through tawcroot, from
// <store>/cmdline): ro.hardware and a locked, verified boot
std::string guest_cmdline(const DeviceIdentity& id);

// `export NAME VALUE` lines of an init.environ.rc
std::vector<std::pair<std::string, std::string>> parse_environ_rc(const std::string& text);

} // namespace vine::rootless
