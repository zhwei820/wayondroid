package dev.probe.env;

import android.app.Activity;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.wifi.WifiManager;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;
import android.os.Bundle;
import android.telephony.ServiceState;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.lang.reflect.Method;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.regex.Pattern;

/** Reports what an ordinary app sees of the device, to files/report.txt. */
public class Main extends Activity {
    static final Pattern MARK = Pattern.compile(
            "(?i)waydroid|lineage|goldfish|ranchu|qemu|cuttlefish|vsoc|gfxstream|virtio|tawc|vine|emulator|"
                    + "genymotion|anbox|redroid|ColorOS|oplus|hexadecinull|reference-ril|kumquat|lavapipe|llvmpipe|LP_NUM_THREADS|mesa");
    final StringBuilder out = new StringBuilder();

    void put(String k, Object v) {
        String s = String.valueOf(v);
        out.append(k).append('=').append(s.replace('\n', '|'));
        if (MARK.matcher(s).find()) out.append("    <<MARK");
        out.append('\n');
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        new Thread(() -> {
            String[] steps = {"build", "props", "telephony", "network", "sensors", "gl", "files", "hw", "packages"};
            for (String s : steps) {
                try {
                    getClass().getDeclaredMethod(s).invoke(this);
                } catch (Throwable t) {
                    put(s + ".error", t.getCause() != null ? t.getCause() : t);
                }
            }
            try (FileOutputStream f = new FileOutputStream(new File(getFilesDir(), "report.txt"))) {
                f.write(out.toString().getBytes());
            } catch (Exception ignored) {
            }
            finish();
        }).start();
    }

    void build() {
        put("Build.MODEL", Build.MODEL); put("Build.BRAND", Build.BRAND); put("Build.DEVICE", Build.DEVICE);
        put("Build.PRODUCT", Build.PRODUCT); put("Build.HARDWARE", Build.HARDWARE); put("Build.BOARD", Build.BOARD);
        put("Build.FINGERPRINT", Build.FINGERPRINT); put("Build.TAGS", Build.TAGS); put("Build.TYPE", Build.TYPE);
        put("Build.HOST", Build.HOST); put("Build.USER", Build.USER); put("Build.DISPLAY", Build.DISPLAY);
        put("Build.SOC", Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL); put("Build.radio", Build.getRadioVersion());
        put("os.version", System.getProperty("os.version"));
    }

    static String prop(String k) {
        try {
            Method m = Class.forName("android.os.SystemProperties").getMethod("get", String.class);
            return (String) m.invoke(null, k);
        } catch (Exception e) {
            return "ERR " + e;
        }
    }

    void props() {
        for (String k : new String[] {"waydroid.display_width", "waydroid.stub_sensors_hal", "ro.lineage.version",
                "ro.modversion", "ro.lineagelegal.url", "sys.lineage_settings_system_version", "ro.board.platform",
                "ro.hardware", "ro.hardware.egl", "ro.hardware.vulkan", "ro.hardware.gralloc", "ro.vendor.vine.gpu",
                "ro.kernel.qemu", "ro.boot.qemu", "ro.product.system.device", "ro.build.user", "ro.bootloader",
                "ro.baseband", "gsm.sim.state", "gsm.version.baseband", "gsm.version.ril-impl", "init.svc.vendor.ril-daemon",
                "ro.secure", "ro.debuggable", "ro.boot.verifiedbootstate", "ro.boot.flash.locked"}) {
            put("prop." + k, prop(k));
        }
    }

    void telephony() {
        TelephonyManager tm = getSystemService(TelephonyManager.class);
        put("tel.simState", tm.getSimState()); put("tel.phoneType", tm.getPhoneType());
        put("tel.networkOperator", tm.getNetworkOperator()); put("tel.networkOperatorName", tm.getNetworkOperatorName());
        put("tel.networkCountryIso", tm.getNetworkCountryIso()); put("tel.simOperator", tm.getSimOperator());
        put("tel.voiceCapable", tm.isVoiceCapable()); put("tel.smsCapable", tm.isSmsCapable());
        put("tel.phoneCount", tm.getActiveModemCount());
        try {
            ServiceState ss = tm.getServiceState();
            put("tel.serviceState", ss == null ? null : ss.getState());
            if (ss != null) put("tel.emergencyOnly", ss.getClass().getMethod("isEmergencyOnly").invoke(ss));
        } catch (Throwable t) {
            put("tel.serviceState", t);
        }
        try {
            put("tel.activeSubs", getSystemService(SubscriptionManager.class).getActiveSubscriptionInfoCount());
        } catch (Throwable t) {
            put("tel.activeSubs", t);
        }
    }

    void network() throws Exception {
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        NetworkInfo ni = cm.getActiveNetworkInfo();
        put("net.info", ni == null ? null : ni.getType() + " " + ni.getTypeName() + " " + ni.getState());
        android.net.NetworkCapabilities ac = cm.getNetworkCapabilities(cm.getActiveNetwork());
        put("net.caps", ac == null ? null : "transportInfo=" + ac.getTransportInfo() + " specifier=" + ac.getNetworkSpecifier());
        Network n = cm.getActiveNetwork();
        NetworkCapabilities nc = n == null ? null : cm.getNetworkCapabilities(n);
        put("net.wifi", nc != null && nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI));
        put("net.ethernet", nc != null && nc.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET));
        put("net.cellular", nc != null && nc.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR));
        LinkProperties lp = n == null ? null : cm.getLinkProperties(n);
        put("net.iface", lp == null ? null : lp.getInterfaceName());
        StringBuilder ifs = new StringBuilder();
        for (NetworkInterface i : Collections.list(NetworkInterface.getNetworkInterfaces())) ifs.append(i.getName()).append(' ');
        put("net.interfaces", ifs);
        WifiManager wm = getSystemService(WifiManager.class);
        put("wifi.enabled", wm.isWifiEnabled());
        android.net.wifi.WifiInfo wi = wm.getConnectionInfo();
        put("wifi.ssid", wi.getSSID());
        StringBuilder scan = new StringBuilder();
        try {
            for (android.net.wifi.ScanResult r : wm.getScanResults()) scan.append(r.SSID).append('/').append(r.level).append(' ');
        } catch (Exception e) {
            scan.append(e);
        }
        put("wifi.scan", scan);
        android.content.Intent sticky = registerReceiver(null,
                new android.content.IntentFilter(android.net.wifi.WifiManager.WIFI_STATE_CHANGED_ACTION));
        put("wifi.sticky", sticky == null ? null : sticky.getIntExtra(android.net.wifi.WifiManager.EXTRA_WIFI_STATE, -1));
        try {
            java.util.concurrent.CountDownLatch got = new java.util.concurrent.CountDownLatch(1);
            boolean[] updated = {false};
            android.os.HandlerThread t = new android.os.HandlerThread("scan");
            t.start();
            android.content.BroadcastReceiver r = new android.content.BroadcastReceiver() {
                @Override public void onReceive(android.content.Context c, android.content.Intent i) {
                    updated[0] = i.getBooleanExtra(android.net.wifi.WifiManager.EXTRA_RESULTS_UPDATED, false);
                    got.countDown();
                }
            };
            registerReceiver(r, new android.content.IntentFilter(android.net.wifi.WifiManager.SCAN_RESULTS_AVAILABLE_ACTION),
                    null, new android.os.Handler(t.getLooper()));
            boolean started = wm.startScan();
            boolean arrived = got.await(5, java.util.concurrent.TimeUnit.SECONDS);
            unregisterReceiver(r);
            t.quit();
            put("wifi.startScan", started + " broadcast=" + (arrived ? "updated=" + updated[0] : "none"));
        } catch (Exception e) {
            put("wifi.startScan", e);
        }
        put("wifi.info", wi.getBSSID() + " rssi=" + wi.getRssi() + " " + wi.getLinkSpeed() + "Mbps " + wi.getFrequency()
                + "MHz std=" + wi.getWifiStandard() + " id=" + wi.getNetworkId() + " " + wi.getSupplicantState()
                + " ip=" + wi.getIpAddress() + " mac=" + wi.getMacAddress());
    }

    void sensors() {
        SensorManager sm = getSystemService(SensorManager.class);
        for (Sensor s : sm.getSensorList(Sensor.TYPE_ALL)) put("sensor." + s.getType(), s.getName() + " | " + s.getVendor());
    }

    void gl() {
        EGLDisplay d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] v = new int[2];
        EGL14.eglInitialize(d, v, 0, v, 1);
        EGLConfig[] cfg = new EGLConfig[1];
        int[] n = new int[1];
        EGL14.eglChooseConfig(d, new int[] {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE}, 0, cfg, 0, 1, n, 0);
        EGLContext c = EGL14.eglCreateContext(d, cfg[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        EGLSurface s = EGL14.eglCreatePbufferSurface(d, cfg[0], new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
        EGL14.eglMakeCurrent(d, s, s, c);
        put("gl.vendor", GLES20.glGetString(GLES20.GL_VENDOR));
        put("gl.renderer", GLES20.glGetString(GLES20.GL_RENDERER));
        put("gl.version", GLES20.glGetString(GLES20.GL_VERSION));
        put("egl.vendor", EGL14.eglQueryString(d, EGL14.EGL_VENDOR));
        EGL14.eglMakeCurrent(d, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(d, s);
        EGL14.eglDestroyContext(d, c);
    }

    static String read(String path, int maxLines) {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            String l;
            for (int i = 0; (l = r.readLine()) != null && i < maxLines; i++) sb.append(l).append('\n');
        } catch (Exception e) {
            return "ERR " + e.getClass().getSimpleName() + ": " + e.getMessage();
        }
        return sb.toString();
    }

    void files() {
        put("proc.version", read("/proc/version", 1));
        put("proc.cpuinfo.hardware", grep(read("/proc/cpuinfo", 400), "Hardware|CPU part"));
        String mounts = read("/proc/self/mounts", 2000);
        put("proc.mounts.lines", mounts.split("\n").length);
        int i = 0;
        for (String l : mounts.split("\n")) {
            if (MARK.matcher(l).find() || l.contains(" /my_") || l.contains(getPackageName())) put("proc.mounts." + i++, l);
        }
        for (String f : new String[] {"/dev/qemu_pipe", "/dev/goldfish_pipe", "/dev/socket/qemud", "/dev/binderfs",
                "/system/lib64/libc_malloc_debug_qemu.so", "/vendor/bin/hw/libgoldfish-rild", "/sys/qemu_trace",
                "/vendor/lib64/hw/gralloc.vine.so", "/vendor/lib64/hw/vulkan.gfxstream.so", "/system/bin/wificond",
                "/vendor/etc/init/vine-radio.rc", "/proc/tty/drivers"}) {
            put("file." + f, new File(f).exists());
        }
        put("proc.tty.drivers", grep(read("/proc/tty/drivers", 100), "goldfish|virtio|ttyS"));
        for (String d : new String[] {"/", "/system", "/system/bin", "/system/bin/hw", "/system/etc/init", "/system/lib64",
                "/system/addon.d", "/vendor", "/vendor/bin/hw", "/vendor/etc/init", "/vendor/lib64", "/vendor/lib64/hw",
                "/product/etc", "/product/etc/init", "/product/etc/permissions", "/system_ext/etc/init",
                "/system_ext/etc/permissions", "/vendor/etc/permissions", "/vendor/etc/vintf/manifest"}) {
            String[] names = new File(d).list();
            StringBuilder m = new StringBuilder();
            for (String n : names == null ? new String[0] : names) if (MARK.matcher(n).find()) m.append(n).append(' ');
            put("ls." + d, names == null ? "unreadable" : names.length + " entries " + m);
        }
        // Listed whole: what a phone has there is known
        for (String d : new String[] {"/run", "/run/user/1000", "/dev/socket", "/dev"}) {
            String[] names = new File(d).list();
            if (names != null) java.util.Arrays.sort(names);
            put("dir." + d, names == null ? "unreadable" : String.join(" ", names));
        }
        // The environment, less what every Android app process has
        StringBuilder env = new StringBuilder();
        for (java.util.Map.Entry<String, String> e : new java.util.TreeMap<>(System.getenv()).entrySet()) {
            if (!e.getKey().matches("PATH|ANDROID_.*|BOOTCLASSPATH|DEX2OATBOOTCLASSPATH|SYSTEMSERVERCLASSPATH|"
                    + "STANDALONE_SYSTEMSERVER_JARS|EXTERNAL_STORAGE|ASEC_MOUNTPOINT|DOWNLOAD_CACHE")) {
                env.append(e.getKey()).append('=').append(e.getValue()).append(' ');
            }
        }
        put("env.extra", env);
        for (String f : new String[] {"/vendor/waydroid.prop", "/system/etc/init/init.waydroid.rc", "/product/etc/vine",
                "/system/bin/vine_lmkd", "/vendor/lib64/libcuttlefish_fs.so", "/system/addon.d/50-lineage.sh"}) {
            put("exists." + f, new File(f).exists());
        }
        // AOSP's plat_property_contexts gives every device the qemu_* property areas
        put("proc.self.maps.marks", grep(read("/proc/self/maps", 5000).replaceAll("(?m)^.*/dev/__properties__/u:object_r:qemu_[a-z_]*prop:s0$", ""), MARK.pattern()));
    }

    void hw() {
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        put("display", dm.widthPixels + "x" + dm.heightPixels + " dpi=" + dm.densityDpi + " xdpi=" + dm.xdpi);
        put("display.mode", getWindowManager().getDefaultDisplay().getMode());
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
        ((android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(mi);
        put("mem.total", mi.totalMem);
        put("proc.meminfo", grep(read("/proc/meminfo", 3), "Mem"));
        android.os.StatFs fs = new android.os.StatFs("/data");
        put("statfs.data", fs.getTotalBytes() + " free=" + fs.getAvailableBytes());
        put("runtime.cpus", Runtime.getRuntime().availableProcessors());
        StringBuilder f = new StringBuilder();
        for (int c = 0; c < 8; c++) {
            String d = "/sys/devices/system/cpu/cpu" + c;
            f.append(c).append(':').append(read(d + "/cpufreq/cpuinfo_max_freq", 1).trim()).append('/')
                    .append(read(d + "/cpufreq/cpuinfo_min_freq", 1).trim()).append(" rel=")
                    .append(read(d + "/cpufreq/related_cpus", 1).trim()).append(" midr=")
                    .append(read(d + "/regs/identification/midr_el1", 1).trim()).append("; ");
        }
        put("cpufreq", f);
        String[] pol = new File("/sys/devices/system/cpu/cpufreq").list();
        if (pol != null) java.util.Arrays.sort(pol);
        put("cpufreq.policies", pol == null ? "unreadable" : String.join(" ", pol));
        put("sys.cpu.possible", read("/sys/devices/system/cpu/possible", 1).trim() + " online=" + read("/sys/devices/system/cpu/online", 1).trim());
        put("proc.cpuinfo.full", read("/proc/cpuinfo", 400));
        put("uname.osrelease", read("/proc/sys/kernel/osrelease", 1));
        try {
            Object os = Class.forName("android.system.Os").getMethod("uname").invoke(null);
            put("uname", os.getClass().getField("release").get(os) + " | " + os.getClass().getField("version").get(os));
        } catch (Exception e) { put("uname", "ERR " + e); }
        put("proc.self.cgroup", read("/proc/self/cgroup", 20));
        put("proc.self.mountinfo.marks", grep(read("/proc/self/mountinfo", 3000), MARK.pattern()));
        put("proc.self.status", grep(read("/proc/self/status", 80), "Seccomp|TracerPid|NoNewPrivs|Cpus_allowed_list"));
        try {
            android.hardware.camera2.CameraManager cm = (android.hardware.camera2.CameraManager) getSystemService(CAMERA_SERVICE);
            put("camera.ids", String.join(",", cm.getCameraIdList()));
        } catch (Exception e) { put("camera.ids", "ERR " + e); }
        StringBuilder feat = new StringBuilder();
        for (android.content.pm.FeatureInfo fi : getPackageManager().getSystemAvailableFeatures())
            if (fi.name != null && fi.name.matches(".*(camera|bluetooth|nfc|fingerprint).*")) feat.append(fi.name).append(' ');
        put("features.hw", feat);
        android.bluetooth.BluetoothAdapter ba = android.bluetooth.BluetoothAdapter.getDefaultAdapter();
        try { put("bt", ba == null ? "null" : ba.isEnabled() + " name=" + ba.getName() + " addr=" + ba.getAddress()); }
        catch (Exception e) { put("bt", "ERR " + e); }
        android.content.Intent bat = registerReceiver(null, new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED));
        if (bat != null) put("battery", "level=" + bat.getIntExtra("level", -1) + " plugged=" + bat.getIntExtra("plugged", -1)
                + " status=" + bat.getIntExtra("status", -1) + " volt=" + bat.getIntExtra("voltage", -1)
                + " temp=" + bat.getIntExtra("temperature", -1) + " tech=" + bat.getStringExtra("technology"));
        put("Build.BOOTLOADER", Build.BOOTLOADER);
        put("prop.revision", prop("ro.revision") + " hw=" + prop("ro.boot.hardware.revision") + " bootmode=" + prop("ro.bootmode"));
        put("gl.egl.prop", prop("ro.hardware.egl"));
    }

    static String grep(String text, String re) {
        Pattern p = Pattern.compile(re);
        StringBuilder sb = new StringBuilder();
        for (String l : text.split("\n")) if (p.matcher(l).find() && sb.length() < 2000) sb.append(l.trim()).append(" ; ");
        return sb.toString();
    }

    void packages() {
        PackageManager pm = getPackageManager();
        int n = 0;
        for (ApplicationInfo a : pm.getInstalledApplications(0)) {
            n++;
            if (MARK.matcher(a.packageName + " " + a.sourceDir).find()) put("pkg." + a.packageName, a.sourceDir);
        }
        put("pkg.count", n);
    }
}
