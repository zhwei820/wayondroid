package com.android.server.wifi;

import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;

import com.android.server.VineWifiInfo;

import java.util.List;
import java.util.function.Supplier;

/**
 * Added to the guest's service-wifi.jar by dev.vine.pmpatch.Main, which
 * points six calls of WifiServiceImpl and ScanRequestProxy's scan
 * failure broadcasts here. The guest has no Wi-Fi radio;
 * it is online through tawcroot's wlan0, which the framework already counts
 * as a Wi-Fi network. Apps asking WifiManager see that network: Wi-Fi on,
 * connected to VineWifiInfo's access point, which scans find with its
 * neighbours. WifiServiceImpl still redacts
 * what a caller may not see (the SSID without location permission, as on
 * any phone).
 */
public final class VineWifi {
    private VineWifi() {}

    /** WifiServiceImpl.getWifiEnabledState's ActiveModeWarden.getWifiState() */
    public static int getWifiState(ActiveModeWarden warden) {
        int state = warden.getWifiState();
        if (state != WifiManager.WIFI_STATE_DISABLED || VineWifiInfo.wlan0() == null) return state;
        VineWifiInfo.announceEnabled();
        return WifiManager.WIFI_STATE_ENABLED;
    }

    /**
     * WifiServiceImpl's WifiThreadRunner.call(supplier, default) in
     * getConnectionInfo (a WifiInfo), getScanResults (a list) and startScan
     * (a Boolean), after their permission checks
     */
    public static Object call(WifiThreadRunner runner, Supplier<Object> supplier, Object onTimeout) {
        Object result = runner.call(supplier, onTimeout);
        android.util.Log.i("VineWifiDbg", "call result=" + (result == null ? "null" : result.getClass().getName()));
        if (result instanceof WifiInfo && ((WifiInfo) result).getSupplicantState() != SupplicantState.COMPLETED) {
            WifiInfo fake = VineWifiInfo.fake();
            if (fake != null) return fake;
        } else if (result instanceof List && ((List<?>) result).isEmpty() && VineWifiInfo.wlan0() != null) {
            return VineWifiInfo.scan();
        } else if ((result == null || result instanceof Boolean) && VineWifiInfo.wlan0() != null) {
            // startScan, which without a radio fails or starts a scan that never ends
            VineWifiInfo.announceScan();
            return Boolean.TRUE;
        }
        return result;
    }

    /**
     * WifiServiceImpl.getCurrentNetwork's ClientMode.syncGetCurrentNetwork():
     * without a radio there is none, and SystemUI's Internet panel shows no
     * connection without its NetworkInfo
     */
    public static android.net.Network syncGetCurrentNetwork(ClientMode mode) {
        android.net.Network network = mode.syncGetCurrentNetwork();
        android.net.Network out = network != null ? network : VineWifiInfo.network();
        android.util.Log.i("VineWifiDbg", "getCurrentNetwork real=" + network + " out=" + out);
        return out;
    }

    /**
     * WifiServiceImpl.getPrivilegedConfiguredNetworks' WifiThreadRunner.call:
     * the saved network VineWifiInfo's connection is to (Settings and
     * SystemUI only show a connection to a saved network)
     */
    @SuppressWarnings("unchecked")
    public static Object callConfigs(WifiThreadRunner runner, Supplier<Object> supplier, Object onTimeout) {
        Object result = runner.call(supplier, onTimeout);
        android.net.wifi.WifiConfiguration config = VineWifiInfo.config();
        if (!(result instanceof List) || config == null) return result;
        for (Object c : (List<Object>) result) {
            if (c instanceof android.net.wifi.WifiConfiguration && ((android.net.wifi.WifiConfiguration) c).networkId == 0)
                return result;
        }
        List<Object> out = new java.util.ArrayList<>((List<Object>) result);
        out.add(config);
        return out;
    }

    /**
     * ScanRequestProxy.startScan's sendScanResultFailureBroadcastToPackage:
     * none while startScan reports VineWifiInfo's scans instead
     */
    public static void sendScanResultFailureBroadcastToPackage(ScanRequestProxy proxy, String packageName) {
        if (VineWifiInfo.wlan0() != null) return;
        try {
            java.lang.reflect.Method m =
                    ScanRequestProxy.class.getDeclaredMethod("sendScanResultFailureBroadcastToPackage", String.class);
            m.setAccessible(true);
            m.invoke(proxy, packageName);
        } catch (Exception e) {
            // no broadcast
        }
    }
}
