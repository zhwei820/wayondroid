package com.android.server.ethernet;

import android.content.Context;
import android.net.LinkProperties;
import android.net.NetworkAgentConfig;
import android.net.NetworkCapabilities;
import android.net.NetworkProvider;
import android.net.NetworkSpecifier;
import android.net.TransportInfo;
import android.os.Looper;

import com.android.server.VineWifiInfo;

/**
 * Added to the guest's service-connectivity.jar by dev.vine.pmpatch.Main,
 * which points EthernetNetworkFactory's makeEthernetNetworkAgent call here.
 * The interface the guest's overlay makes a Wi-Fi network gets a Wi-Fi
 * network's capabilities: VineWifiInfo's WifiInfo as transport info (which
 * ConnectivityService redacts per app, as on any phone) and no Ethernet
 * specifier.
 */
public final class VineEthernet {
    private VineEthernet() {}

    public static EthernetNetworkAgent makeEthernetNetworkAgent(EthernetNetworkFactory.Dependencies deps,
            Context context, Looper looper, NetworkCapabilities nc, LinkProperties lp, NetworkAgentConfig config,
            NetworkProvider provider, EthernetNetworkAgent.Callbacks cb) {
        return deps.makeEthernetNetworkAgent(context, looper, asWifi(nc), lp, config, provider, cb);
    }

    static NetworkCapabilities asWifi(NetworkCapabilities nc) {
        if (!nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return nc;
        try {
            // Hidden setters, which the framework's own builder calls
            NetworkCapabilities wifi = new NetworkCapabilities(nc);
            NetworkCapabilities.class.getMethod("setNetworkSpecifier", NetworkSpecifier.class).invoke(wifi, (Object) null);
            TransportInfo info = VineWifiInfo.fake();
            if (info != null) {
                NetworkCapabilities.class.getMethod("setTransportInfo", TransportInfo.class).invoke(wifi, info);
            }
            return wifi;
        } catch (Exception e) {
            return nc;
        }
    }
}
