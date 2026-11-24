package dev.probe.env;

import android.app.Activity;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
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
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.telephony.ServiceState;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.lang.reflect.Method;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.regex.Pattern;

/** Reports what an ordinary app sees of the device, to files/report.txt, then shows live sensor readings until
 * its close button is tapped (files/closed). */
public class Main extends Activity {
    static final Pattern MARK = Pattern.compile(
            "(?i)waydroid|lineage|goldfish|ranchu|qemu|cuttlefish|vsoc|gfxstream|virtio|tawc|vine|emulator|"
                    + "genymotion|anbox|redroid|ColorOS|oplus|hexadecinull|reference-ril|kumquat|lavapipe|llvmpipe|LP_NUM_THREADS|mesa");
    final StringBuilder out = new StringBuilder();

    void put(String k, Object v) {
        String s = String.valueOf(v);
        out.append(k).append('=').append(s.replace('\n', '|'));
        // the key too for packages: the name is the key
        if (MARK.matcher(s).find() || (k.startsWith("pkg.") && MARK.matcher(k).find())) out.append("    <<MARK");
        out.append('\n');
    }

    static final class Reading {
        int events;
        float[] values;
    }

    /** Every non-one-shot sensor, from onCreate until closed */
    final Map<Sensor, Reading> live = new LinkedHashMap<>();
    final long liveSince = System.currentTimeMillis();
    SensorManager liveManager;
    HandlerThread liveThread;
    final Handler ui = new Handler(Looper.getMainLooper());
    TextView status, readings;
    boolean closed;

    final SensorEventListener liveListener = new SensorEventListener() {
        @Override public void onSensorChanged(SensorEvent e) {
            synchronized (live) {
                Reading r = live.get(e.sensor);
                if (r == null) return;
                r.events++;
                r.values = e.values.clone();
            }
        }
        @Override public void onAccuracyChanged(Sensor s, int a) {}
    };

    static String values(float[] v) {
        if (v == null) return "-";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < Math.min(3, v.length); i++) b.append(i > 0 ? " " : "").append(String.format("%.3f", v[i]));
        return b.toString();
    }

    void startLive() {
        liveManager = getSystemService(SensorManager.class);
        liveThread = new HandlerThread("live");
        liveThread.start();
        Handler h = new Handler(liveThread.getLooper());
        for (Sensor s : liveManager.getSensorList(Sensor.TYPE_ALL)) {
            if (s.getReportingMode() == Sensor.REPORTING_MODE_ONE_SHOT) continue;
            synchronized (live) { live.put(s, new Reading()); }
            liveManager.registerListener(liveListener, s, SensorManager.SENSOR_DELAY_UI, h);
        }
        ui.post(new Runnable() {
            @Override public void run() {
                if (closed) return;
                StringBuilder b = new StringBuilder();
                synchronized (live) {
                    for (Map.Entry<Sensor, Reading> en : live.entrySet())
                        b.append(en.getKey().getName()).append("\n    ").append(values(en.getValue().values))
                                .append("   (").append(en.getValue().events).append(" 次)\n");
                }
                readings.setText(b);
                ui.postDelayed(this, 250);
            }
        });
    }

    void close() {
        if (closed) return;
        closed = true;
        if (liveManager != null) liveManager.unregisterListener(liveListener);
        if (liveThread != null) liveThread.quitSafely();
        try {
            new File(getFilesDir(), "closed").createNewFile();
        } catch (Exception ignored) {
        }
    }

    @Override protected void onDestroy() {
        close();
        super.onDestroy();
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(32, 32, 32, 32);
        status = new TextView(this);
        status.setText("探测中…");
        Button close = new Button(this);
        close.setText("关闭");
        close.setOnClickListener(v -> {
            close();
            finishAndRemoveTask();
        });
        readings = new TextView(this);
        readings.setTypeface(android.graphics.Typeface.MONOSPACE);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(readings);
        root.addView(status);
        root.addView(close);
        root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);
        startLive();
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
            ui.post(() -> status.setText("报告已写入。看完读数后点「关闭」结束。"));
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
        for (Sensor s : sm.getSensorList(Sensor.TYPE_ALL))
            put("sensor." + s.getType() + (s.isWakeUpSensor() ? ".wakeup" : ""),
                s.getName() + " | " + s.getVendor() + " | mode=" + s.getReportingMode());
        /* what the live listeners got in their first seconds; on-change sensors only report changes */
        long wait = liveSince + 6000 - System.currentTimeMillis();
        if (wait > 0) {
            try {
                Thread.sleep(wait);
            } catch (InterruptedException ignored) {
            }
        }
        synchronized (live) {
            for (Map.Entry<Sensor, Reading> en : live.entrySet()) {
                Sensor s = en.getKey();
                put("sensor.read." + s.getType() + (s.isWakeUpSensor() ? ".wakeup" : ""),
                        s.getName() + " | events=" + en.getValue().events + " | " + values(en.getValue().values));
            }
        }
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
        String ext = GLES20.glGetString(GLES20.GL_EXTENSIONS);
        int[] numExt = new int[1];
        GLES20.glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, numExt, 0);
        put("gl.extensions", (ext == null ? 0 : ext.split(" ").length) + " num=" + numExt[0]
                + " angle=" + (ext != null && ext.matches("(?s).*GL_(ANGLE|CHROMIUM)_.*")));
        put("egl.vendor", EGL14.eglQueryString(d, EGL14.EGL_VENDOR));
        StringBuilder want = new StringBuilder();
        for (String e : new String[] {"GL_EXT_geometry_shader", "GL_OES_geometry_shader", "GL_EXT_tessellation_shader",
                "GL_OES_tessellation_shader", "GL_EXT_gpu_shader5", "GL_OES_gpu_shader5", "GL_KHR_blend_equation_advanced",
                "GL_OES_sample_shading", "GL_OES_texture_buffer", "GL_EXT_texture_cube_map_array"})
            want.append(e).append(ext != null && (" " + ext + " ").contains(" " + e + " ") ? "=1 " : "=0 ");
        put("gl.es32ext", want.toString());
        EGL14.eglMakeCurrent(d, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(d, s);
        EGL14.eglDestroyContext(d, c);
        /* What an app asking for 3.2 gets (EGL_CONTEXT_MAJOR/MINOR_VERSION_KHR) */
        EGLContext c32 = EGL14.eglCreateContext(d, cfg[0], EGL14.EGL_NO_CONTEXT,
                new int[] {0x3098, 3, 0x30FB, 2, EGL14.EGL_NONE}, 0);
        put("gl.context32", c32 == null || c32 == EGL14.EGL_NO_CONTEXT ? "fail 0x" + Integer.toHexString(EGL14.eglGetError()) : "ok");
        if (c32 != null && c32 != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(d, c32);
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
        // run.sh drops output lines naming linkerconfig (its exec channel's noise)
        put("proc.mounts.all", mounts.replace("linkerconfig", "linker_config"));
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
        for (String camId : new String[] {"0", "1"}) put("camera.capture." + camId, capture(camId));
        try {
            android.hardware.camera2.CameraManager cm2 = (android.hardware.camera2.CameraManager) getSystemService(CAMERA_SERVICE);
            for (String camId : cm2.getCameraIdList()) {
                android.hardware.camera2.CameraCharacteristics cc = cm2.getCameraCharacteristics(camId);
                android.hardware.camera2.params.StreamConfigurationMap map =
                        cc.get(android.hardware.camera2.CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                android.util.Size[] jpeg = map == null ? null : map.getOutputSizes(android.graphics.ImageFormat.JPEG);
                put("camera.chars." + camId, "facing=" + cc.get(android.hardware.camera2.CameraCharacteristics.LENS_FACING)
                        + " level=" + cc.get(android.hardware.camera2.CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)
                        + " pixelArray=" + cc.get(android.hardware.camera2.CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
                        + " physical=" + cc.get(android.hardware.camera2.CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
                        + " focal=" + java.util.Arrays.toString(cc.get(android.hardware.camera2.CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS))
                        + " orientation=" + cc.get(android.hardware.camera2.CameraCharacteristics.SENSOR_ORIENTATION)
                        + " caps=" + java.util.Arrays.toString(cc.get(android.hardware.camera2.CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES))
                        + " maxJpeg=" + (jpeg == null || jpeg.length == 0 ? "none" : jpeg[0])
                        + " physicalIds=" + cc.getPhysicalCameraIds());
            }
        } catch (Exception e) { put("camera.chars", "ERR " + e); }
        StringBuilder modes = new StringBuilder();
        for (android.view.Display.Mode m : getWindowManager().getDefaultDisplay().getSupportedModes())
            modes.append(m.getPhysicalWidth()).append('x').append(m.getPhysicalHeight()).append('@').append(m.getRefreshRate()).append(' ');
        put("display.modes", modes);
        TelephonyManager tmx = getSystemService(TelephonyManager.class);
        try { put("tel.modems", "supported=" + tmx.getSupportedModemCount() + " active=" + tmx.getActiveModemCount()); }
        catch (Exception e) { put("tel.modems", "ERR " + e); }
        // With FLAG_INCLUDE_LOCATION_INFO the network's WifiInfo is the one getConnectionInfo gives an app
        // with location access; without it, redacted (see net.caps)
        try {
            android.net.ConnectivityManager cmx = getSystemService(android.net.ConnectivityManager.class);
            final String[] got = {"none"};
            final java.util.concurrent.CountDownLatch l = new java.util.concurrent.CountDownLatch(1);
            android.net.ConnectivityManager.NetworkCallback cb = new android.net.ConnectivityManager.NetworkCallback(
                    android.net.ConnectivityManager.NetworkCallback.FLAG_INCLUDE_LOCATION_INFO) {
                @Override public void onCapabilitiesChanged(android.net.Network n, android.net.NetworkCapabilities c) {
                    Object ti = c.getTransportInfo();
                    if (ti instanceof android.net.wifi.WifiInfo) {
                        android.net.wifi.WifiInfo wi = (android.net.wifi.WifiInfo) ti;
                        got[0] = wi.getSSID() + " " + wi.getBSSID() + " id=" + wi.getNetworkId();
                    }
                    l.countDown();
                }
            };
            cmx.registerDefaultNetworkCallback(cb);
            l.await(5, java.util.concurrent.TimeUnit.SECONDS);
            cmx.unregisterNetworkCallback(cb);
            put("net.caps.withLocation", got[0]);
        } catch (Exception e) { put("net.caps.withLocation", "ERR " + e); }
        try {
            SensorManager smx = getSystemService(SensorManager.class);
            Sensor p = smx.getDefaultSensor(Sensor.TYPE_PRESSURE);
            final float[] v = {Float.NaN};
            final java.util.concurrent.CountDownLatch l = new java.util.concurrent.CountDownLatch(1);
            android.hardware.SensorEventListener sl = new android.hardware.SensorEventListener() {
                @Override public void onSensorChanged(android.hardware.SensorEvent e) { v[0] = e.values[0]; l.countDown(); }
                @Override public void onAccuracyChanged(Sensor s, int a) {}
            };
            if (p != null) {
                android.os.HandlerThread sh = new android.os.HandlerThread("baro");
                sh.start();
                smx.registerListener(sl, p, SensorManager.SENSOR_DELAY_NORMAL, new android.os.Handler(sh.getLooper()));
                l.await(5, java.util.concurrent.TimeUnit.SECONDS);
                smx.unregisterListener(sl);
                sh.quitSafely();
            }
            put("sensor.pressure", p == null ? "none" : p.getName() + " | " + p.getVendor() + " = " + v[0] + " hPa");
        } catch (Exception e) { put("sensor.pressure", "ERR " + e); }
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
        try (java.io.FileInputStream in = new java.io.FileInputStream("/proc/self/auxv")) {
            byte[] b = new byte[4096];
            int len = in.read(b);
            java.nio.ByteBuffer bb = java.nio.ByteBuffer.wrap(b, 0, Math.max(len, 0)).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            StringBuilder a = new StringBuilder();
            while (bb.remaining() >= 16) {
                long t = bb.getLong(), v = bb.getLong();
                if (t == 0) break;
                if (t == 16 || t == 26 || t == 9 || t == 3) a.append(t).append('=').append(Long.toHexString(v)).append(' ');
            }
            put("auxv", a);
        } catch (Exception e) { put("auxv", "ERR " + e); }
        put("Build.BOOTLOADER", Build.BOOTLOADER);
        put("prop.revision", prop("ro.revision") + " hw=" + prop("ro.boot.hardware.revision") + " bootmode=" + prop("ro.bootmode"));
        put("gl.egl.prop", prop("ro.hardware.egl"));
    }

    /** Opens a camera and takes one 640x480 JPEG: "jpeg <bytes> <w>x<h>" or what failed */
    String capture(String id) {
        android.os.HandlerThread ht = new android.os.HandlerThread("cam" + id);
        ht.start();
        android.os.Handler h = new android.os.Handler(ht.getLooper());
        final String[] result = {"timeout"};
        final java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        final android.hardware.camera2.CameraDevice[] dev = {null};
        /* stream a while first: the camera may show a placeholder until it is ready */
        final long[] since = {Long.MAX_VALUE};
        final int[] frames = {0};
        final android.hardware.camera2.CameraCaptureSession[] session = {null};
        try {
            android.hardware.camera2.CameraManager cm = (android.hardware.camera2.CameraManager) getSystemService(CAMERA_SERVICE);
            final android.media.ImageReader reader = android.media.ImageReader.newInstance(640, 480, android.graphics.ImageFormat.JPEG, 2);
            reader.setOnImageAvailableListener(r -> {
                android.media.Image img = r.acquireLatestImage();
                if (img != null && (++frames[0] < 2 || android.os.SystemClock.uptimeMillis() - since[0] < 3000)) {
                    img.close();
                } else if (img != null && done.getCount() > 0) {
                    java.nio.ByteBuffer b = img.getPlanes()[0].getBuffer();
                    byte[] jpeg = new byte[b.remaining()];
                    b.get(jpeg);
                    android.graphics.Bitmap bm = android.graphics.BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
                    result[0] = "frames=" + frames[0] + " jpeg " + jpeg.length + " " + (bm == null ? "undecodable" : bm.getWidth() + "x" + bm.getHeight()
                            + " px(320,240)=" + Integer.toHexString(bm.getPixel(320, 240)));
                    img.close();
                    done.countDown();
                }
            }, h);
            cm.openCamera(id, new android.hardware.camera2.CameraDevice.StateCallback() {
                @Override public void onOpened(android.hardware.camera2.CameraDevice d) {
                    dev[0] = d;
                    try {
                        d.createCaptureSession(java.util.Collections.singletonList(reader.getSurface()),
                                new android.hardware.camera2.CameraCaptureSession.StateCallback() {
                                    @Override public void onConfigured(android.hardware.camera2.CameraCaptureSession sess) {
                                        session[0] = sess;
                                        try {
                                            android.hardware.camera2.CaptureRequest.Builder rb =
                                                    d.createCaptureRequest(android.hardware.camera2.CameraDevice.TEMPLATE_PREVIEW);
                                            rb.addTarget(reader.getSurface());
                                            since[0] = android.os.SystemClock.uptimeMillis();
                                            sess.setRepeatingRequest(rb.build(), null, h);
                                        } catch (Exception e) { result[0] = "capture " + e; done.countDown(); }
                                    }
                                    @Override public void onConfigureFailed(android.hardware.camera2.CameraCaptureSession sess) {
                                        result[0] = "configure failed"; done.countDown();
                                    }
                                }, h);
                    } catch (Exception e) { result[0] = "session " + e; done.countDown(); }
                }
                @Override public void onDisconnected(android.hardware.camera2.CameraDevice d) { result[0] = "disconnected"; done.countDown(); }
                @Override public void onError(android.hardware.camera2.CameraDevice d, int e) { result[0] = "error " + e; done.countDown(); }
            }, h);
            done.await(15, java.util.concurrent.TimeUnit.SECONDS);
        } catch (Exception e) {
            result[0] = "ERR " + e;
        } finally {
            try {
                if (session[0] != null) {
                    session[0].stopRepeating();
                    session[0].close();
                }
            } catch (Exception ignored) {
            }
            if (dev[0] != null) dev[0].close();
            ht.quitSafely();
        }
        return result[0];
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
