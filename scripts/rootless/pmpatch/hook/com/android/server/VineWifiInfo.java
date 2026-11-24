package com.android.server;

import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;

import java.lang.reflect.Method;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.Collections;

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

    /** A connected WifiInfo, or null without wlan0 */
    public static WifiInfo fake() {
        NetworkInterface nif = wlan0();
        if (nif == null) return null;
        try {
            byte[] mac = nif.getHardwareAddress();
            if (mac == null || mac.length != 6) return null;
            int h = 0;
            for (byte b : mac) h = h * 31 + (b & 0xff);
            String ssid = String.format("Xiaomi_%04X", h & 0xffff);
            String bssid = String.format("64:09:80:%02x:%02x:%02x", (h >>> 16) & 0xff, (h >>> 8) & 0xff, h & 0xff);
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
            return info;
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
