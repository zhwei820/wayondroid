package com.android.server.wifi;

import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;

import com.android.server.VineWifiInfo;

import java.util.List;
import java.util.function.Supplier;

/**
 * Added to the guest's service-wifi.jar by dev.vine.pmpatch.Main, which
 * points four calls of WifiServiceImpl here. The guest has no Wi-Fi radio;
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
}
