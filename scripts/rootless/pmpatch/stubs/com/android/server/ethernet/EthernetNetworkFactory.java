package com.android.server.ethernet;
import android.content.Context;
import android.net.LinkProperties;
import android.net.NetworkAgentConfig;
import android.net.NetworkCapabilities;
import android.net.NetworkProvider;
import android.os.Looper;
public class EthernetNetworkFactory {
    public static class Dependencies {
        public EthernetNetworkAgent makeEthernetNetworkAgent(Context context, Looper looper, NetworkCapabilities nc,
                LinkProperties lp, NetworkAgentConfig config, NetworkProvider provider, EthernetNetworkAgent.Callbacks cb) {
            throw new RuntimeException();
        }
    }
}
