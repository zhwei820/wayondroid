package com.android.server;

import android.content.Context;
import android.content.Intent;
import android.net.wifi.ScanResult;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.UserHandle;

import java.lang.reflect.Method;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Random;

/**
 * The access point the guest's hooks (VineWifi, VineEthernet) report it is
 * connected to through tawcroot's wlan0: named after wlan0's MAC, so the
 * same for an instance's life. Each hook's dex carries its own copy.
 */
public final class VineWifiInfo {
    private VineWifiInfo() {}

    /** wlan0 when it has an IPv4 address, else null */
    public static NetworkInterface wlan0() {
        try {
            NetworkInterface nif = NetworkInterface.getByName("wlan0");
            if (nif == null || !nif.isUp()) return null;
            for (InetAddress a : Collections.list(nif.getInetAddresses())) {
                if (a instanceof Inet4Address) return nif;
            }
        } catch (Exception e) {
            // no wlan0
        }
        return null;
    }

    /** wlan0's MAC hashed, the seed of every name here; 0 without wlan0 */
    static int seed(NetworkInterface nif) throws Exception {
        byte[] mac = nif == null ? null : nif.getHardwareAddress();
        if (mac == null || mac.length != 6) return 0;
        int h = 0;
        for (byte b : mac) h = h * 31 + (b & 0xff);
        return h == 0 ? 1 : h;
    }

    static String ssid(int h) {
        return String.format("Xiaomi_%04X", h & 0xffff);
    }

    /** The 5 GHz radio's; the 2.4 GHz one's ends one lower */
    static String bssid(int h) {
        return String.format("64:09:80:%02x:%02x:%02x", (h >>> 16) & 0xff, (h >>> 8) & 0xff, h & 0xff);
    }

    /** A connected WifiInfo, or null without wlan0 */
    public static WifiInfo fake() {
        NetworkInterface nif = wlan0();
        if (nif == null) return null;
        try {
            int h = seed(nif);
            if (h == 0) return null;
            byte[] mac = nif.getHardwareAddress();
            String ssid = ssid(h), bssid = bssid(h);
            WifiInfo info = new WifiInfo.Builder()
                    .setSsid(ssid.getBytes(StandardCharsets.UTF_8))
                    .setBssid(bssid)
                    .setNetworkId(0)
                    .setRssi(-52)
                    .build();
            InetAddress ip = null;
            for (InetAddress a : Collections.list(nif.getInetAddresses())) {
                if (a instanceof Inet4Address) ip = a;
            }
            String macText = String.format("%02x:%02x:%02x:%02x:%02x:%02x",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            // Hidden setters, as the framework's own code fills WifiInfo
            set(info, "setSupplicantState", SupplicantState.class, SupplicantState.COMPLETED);
            set(info, "setFrequency", int.class, 5180);
            set(info, "setLinkSpeed", int.class, 866);
            set(info, "setTxLinkSpeedMbps", int.class, 866);
            set(info, "setRxLinkSpeedMbps", int.class, 780);
            set(info, "setMaxSupportedTxLinkSpeedMbps", int.class, 866);
            set(info, "setMaxSupportedRxLinkSpeedMbps", int.class, 866);
            set(info, "setWifiStandard", int.class, 5); // ScanResult.WIFI_STANDARD_11AC
            set(info, "setMacAddress", String.class, macText);
            set(info, "setInetAddress", InetAddress.class, ip);
            // The primary network, as wifitrackerlib shows only that one as
            // connected; PSK, as config() and scan() have it
            set(info, "setIsPrimary", boolean.class, true);
            set(info, "setCurrentSecurityType", int.class, 2); // WifiInfo.SECURITY_TYPE_PSK
            return info;
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * A scan: the access point's two radios and the neighbours' (names a
     * Chinese apartment block shows), the same for an instance's life; empty
     * without wlan0
     */
    public static List<ScanResult> scan() {
        List<ScanResult> out = new ArrayList<>();
        try {
            int h = seed(wlan0());
            if (h == 0) return out;
            Random r = new Random(h);
            String b5 = bssid(h);
            String b24 = b5.substring(0, 15) + String.format("%02x", (Integer.parseInt(b5.substring(15), 16) + 255) & 0xff);
            out.add(result(ssid(h), b5, -52, 5180, ScanResult.CHANNEL_WIDTH_80MHZ, 5210));
            out.add(result(ssid(h), b24, -47, 2437, ScanResult.CHANNEL_WIDTH_20MHZ, 0));
            String[][] neighbours = {
                {"ChinaNet-%04X", "d0:60:8c"}, {"CMCC-%04X", "f4:2e:7f"}, {"TP-LINK_%04X", "50:3e:aa"},
                {"HUAWEI-%04X", "e0:24:7f"}, {"ChinaUnicom-%04X", "c8:3a:35"},
            };
            int[] channels = {2412, 2437, 2462, 2422, 2452};
            for (int i = 0; i < neighbours.length; i++) {
                if (r.nextInt(5) == 0) continue;
                String bssid = String.format("%s:%02x:%02x:%02x", neighbours[i][1], r.nextInt(256), r.nextInt(256),
                        r.nextInt(256));
                out.add(result(String.format(neighbours[i][0], r.nextInt(0x10000)), bssid, -68 - r.nextInt(22),
                        channels[r.nextInt(channels.length)], ScanResult.CHANNEL_WIDTH_20MHZ, 0));
            }
        } catch (Exception e) {
            // as far as it got
        }
        return out;
    }

    static ScanResult result(String ssid, String bssid, int level, int frequency, int width, int center) {
        ScanResult s = new ScanResult();
        s.SSID = ssid;
        s.BSSID = bssid;
        s.capabilities = "[WPA2-PSK-CCMP][RSN-PSK-CCMP][ESS][WPS]";
        s.level = level;
        s.frequency = frequency;
        s.channelWidth = width;
        s.centerFreq0 = center;
        s.timestamp = SystemClock.elapsedRealtime() * 1000;
        try {
            Class<?> ws = Class.forName("android.net.wifi.WifiSsid");
            Object w = ws.getMethod("fromBytes", byte[].class).invoke(null, (Object) ssid.getBytes(StandardCharsets.UTF_8));
            Method m = ScanResult.class.getDeclaredMethod("setWifiSsid", ws);
            m.setAccessible(true);
            m.invoke(s, w);
        } catch (Exception e) {
            // SSID alone
        }
        return s;
    }

    /**
     * The saved network fake()'s connection is to: same network id, SSID
     * and security (PSK, as scan() reports it), as Settings and SystemUI
     * match a connection to a saved network; null without wlan0
     */
    public static android.net.wifi.WifiConfiguration config() {
        try {
            int h = seed(wlan0());
            if (h == 0) return null;
            android.net.wifi.WifiConfiguration c = new android.net.wifi.WifiConfiguration();
            c.networkId = 0;
            c.SSID = "\"" + ssid(h) + "\"";
            c.setSecurityParams(android.net.wifi.WifiConfiguration.SECURITY_TYPE_PSK);
            c.status = android.net.wifi.WifiConfiguration.Status.CURRENT;
            return c;
        } catch (Exception e) {
            return null;
        }
    }

    /** The network wlan0 gives: the default one, which has the Wi-Fi transport; or null */
    public static android.net.Network network() {
        Context context = systemContext();
        if (context == null || wlan0() == null) return null;
        // As system_server, not as the binder caller WifiServiceImpl serves
        long token = android.os.Binder.clearCallingIdentity();
        try {
            android.net.ConnectivityManager cm = context.getSystemService(android.net.ConnectivityManager.class);
            for (android.net.Network n : cm.getAllNetworks()) {
                android.net.NetworkCapabilities caps = cm.getNetworkCapabilities(n);
                if (caps != null && caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)) return n;
            }
        } catch (Exception e) {
            // none
        } finally {
            android.os.Binder.restoreCallingIdentity(token);
        }
        return null;
    }

    /** Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT (hidden), as WifiService sends these */
    private static final int FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT = 0x04000000;

    private static volatile boolean announced;

    /**
     * WifiManager's sticky state broadcast as Wi-Fi enabled, once per class
     * loader: WifiService leaves it disabled, and apps read it on registering
     */
    public static void announceEnabled() {
        if (announced || wlan0() == null) return;
        Context context = systemContext();
        if (context == null) return;
        announced = true;
        Intent i = new Intent(WifiManager.WIFI_STATE_CHANGED_ACTION);
        i.addFlags(FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT);
        i.putExtra(WifiManager.EXTRA_WIFI_STATE, WifiManager.WIFI_STATE_ENABLED);
        i.putExtra(WifiManager.EXTRA_PREVIOUS_WIFI_STATE, WifiManager.WIFI_STATE_DISABLED);
        try {
            context.sendStickyBroadcastAsUser(i, all());
        } catch (Exception e) {
            announced = false;
        }
    }

    /** A scan's end, as a scan takes a moment: SCAN_RESULTS_AVAILABLE, results updated */
    public static void announceScan() {
        Context context = systemContext();
        if (context == null) return;
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            Intent i = new Intent(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION);
            i.addFlags(FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT);
            i.putExtra(WifiManager.EXTRA_RESULTS_UPDATED, true);
            try {
                context.sendBroadcastAsUser(i, all());
            } catch (Exception e) {
                // no broadcast
            }
        }, 2000);
    }

    static UserHandle all() throws Exception {
        return (UserHandle) UserHandle.class.getField("ALL").get(null);
    }

    /** system_server's context (hidden: ActivityThread.getSystemContext) */
    static Context systemContext() {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getMethod("currentActivityThread").invoke(null);
            return thread == null ? null : (Context) at.getMethod("getSystemContext").invoke(thread);
        } catch (Exception e) {
            return null;
        }
    }

    static void set(WifiInfo info, String name, Class<?> type, Object value) {
        try {
            Method m = WifiInfo.class.getDeclaredMethod(name, type);
            m.setAccessible(true);
            m.invoke(info, value);
        } catch (Exception e) {
            // absent on this release: left at its default
        }
    }
}
