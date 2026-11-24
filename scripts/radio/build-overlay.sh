#!/bin/sh
# Builds the guest's static overlays into app/src/main/jniLibs/<abi>/ (APKs under library names;
# rootless_prep installs them into /vendor/overlay). Gradle runs it before every build.
#
# libvine_telephony_overlay.so (VineTelephony.apk, next to the RIL): Waydroid's framework says
# the device can neither call, text nor use mobile data, and Android 13's RIL then never talks
# to the radio HAL. The overlay makes it all three, as the phone it poses as (Pixel 5) is.
#
# libvine_wifinet_overlay.so (VineWifiNet.apk): EthernetTracker takes tawcroot's wlan0 and
# reports it as Wi-Fi (transport 1), legacy type TYPE_WIFI included; the capabilities are
# Ethernet's defaults, which the listed ones must repeat (NOT_VCN_MANAGED among them, or no
# request matches and the guest has no default network).
#
# Needs the Android SDK (build-tools with aapt2 and apksigner, a platform's android.jar) and keytool.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="${ANDROID_HOME:-$(sed -n 's/^sdk.dir=//p' "$root/local.properties" 2>/dev/null)}"
bt="$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)"
jar="$(ls -d "$sdk"/platforms/android-*/android.jar | sort -V | tail -1)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Any key will do: a static overlay on /vendor is trusted for its partition, not its signer
keytool -genkeypair -keystore "$work/key.jks" -storepass overlay -keypass overlay -alias overlay \
    -keyalg RSA -keysize 2048 -validity 10000 -dname CN=VineOverlay >/dev/null 2>&1

# overlay LIBNAME PACKAGE OVERLAY_ATTRS, the values XML on stdin
overlay() {
    d="$work/$1"
    mkdir -p "$d/res/values"
    cat > "$d/res/values/config.xml"
    cat > "$d/AndroidManifest.xml" <<EOF
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="$2">
    <overlay $3 android:isStatic="true" android:priority="1" />
    <application android:hasCode="false" />
</manifest>
EOF
    "$bt/aapt2" compile --dir "$d/res" -o "$d/res.zip"
    "$bt/aapt2" link -o "$d/unsigned.apk" -I "$jar" --manifest "$d/AndroidManifest.xml" \
        --min-sdk-version 29 --target-sdk-version 33 "$d/res.zip"
    "$bt/apksigner" sign --ks "$work/key.jks" --ks-pass pass:overlay --out "$d/overlay.apk" "$d/unsigned.apk"
    for abi in arm64-v8a x86_64; do
        mkdir -p "$root/app/src/main/jniLibs/$abi"
        cp "$d/overlay.apk" "$root/app/src/main/jniLibs/$abi/$1"
    done
    echo "app/src/main/jniLibs/*/$1"
}

overlay libvine_telephony_overlay.so com.hexadecinull.vineos.overlay.telephony \
    'android:targetPackage="android"' <<'EOF'
<resources>
    <bool name="config_mobile_data_capable">true</bool>
    <bool name="config_voice_capable">true</bool>
    <bool name="config_sms_capable">true</bool>
</resources>
EOF

# Capabilities: NOT_METERED 11, INTERNET 12, NOT_RESTRICTED 13, TRUSTED 14, NOT_VPN 15,
# NOT_ROAMING 18, NOT_CONGESTED 20, NOT_SUSPENDED 21, NOT_VCN_MANAGED 28
overlay libvine_wifinet_overlay.so com.hexadecinull.vineos.overlay.wifinet \
    'android:targetPackage="com.android.connectivity.resources" android:targetName="ServiceConnectivityResourcesConfig"' <<'EOF'
<resources>
    <string name="config_ethernet_iface_regex" translatable="false">wlan\\d</string>
    <string-array name="config_ethernet_interfaces" translatable="false">
        <item>wlan0;11,12,13,14,15,18,20,21,28;;1</item>
    </string-array>
</resources>
EOF
