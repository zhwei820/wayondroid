package com.android.server.wifi;

import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;

import com.android.server.VineWifiInfo;

import java.util.function.Supplier;

/**
 * Added to the guest's service-wifi.jar by dev.vine.pmpatch.Main, which
 * points two calls of WifiServiceImpl here. The guest has no Wi-Fi radio;
 * it is online through tawcroot's wlan0, which the framework already counts
 * as a Wi-Fi network. Apps asking WifiManager see that network: Wi-Fi on,
 * connected to VineWifiInfo's access point. WifiServiceImpl still redacts
 * what a caller may not see (the SSID without location permission, as on
 * any phone).
 */
public final class VineWifi {
    private VineWifi() {}

    /** WifiServiceImpl.getWifiEnabledState's ActiveModeWarden.getWifiState() */
    public static int getWifiState(ActiveModeWarden warden) {
        int state = warden.getWifiState();
        return state == WifiManager.WIFI_STATE_DISABLED && VineWifiInfo.wlan0() != null ? WifiManager.WIFI_STATE_ENABLED : state;
    }

    /** WifiServiceImpl.getConnectionInfo's WifiThreadRunner.call(supplier, new WifiInfo()) */
    public static Object call(WifiThreadRunner runner, Supplier<Object> supplier, Object onTimeout) {
        Object info = runner.call(supplier, onTimeout);
        if (info instanceof WifiInfo && ((WifiInfo) info).getSupplicantState() != SupplicantState.COMPLETED) {
            WifiInfo fake = VineWifiInfo.fake();
            if (fake != null) return fake;
        }
        return info;
    }
}
