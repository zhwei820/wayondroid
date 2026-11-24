#!/bin/sh
# Builds the guest's telephony overlay into app/src/main/jniLibs/<abi>/libvine_telephony_overlay.so
# (an APK; rootless_prep installs it as /vendor/overlay/VineTelephony.apk next to the RIL).
#
# Waydroid's framework says the device can neither call, text nor use mobile data, and
# Android 13's RIL then never talks to the radio HAL. The overlay makes it all three, as
# the phone it poses as (Pixel 5) is.
#
# Needs the Android SDK (build-tools with aapt2 and apksigner, a platform's android.jar) and keytool.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
sdk="${ANDROID_HOME:-$(sed -n 's/^sdk.dir=//p' "$root/local.properties" 2>/dev/null)}"
bt="$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)"
jar="$(ls -d "$sdk"/platforms/android-*/android.jar | sort -V | tail -1)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/res/values"
cat > "$work/AndroidManifest.xml" <<'EOF'
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.hexadecinull.vineos.overlay.telephony">
    <overlay android:targetPackage="android" android:isStatic="true" android:priority="1" />
    <application android:hasCode="false" />
</manifest>
EOF
cat > "$work/res/values/config.xml" <<'EOF'
<resources>
    <bool name="config_mobile_data_capable">true</bool>
    <bool name="config_voice_capable">true</bool>
    <bool name="config_sms_capable">true</bool>
</resources>
EOF
"$bt/aapt2" compile --dir "$work/res" -o "$work/res.zip"
"$bt/aapt2" link -o "$work/unsigned.apk" -I "$jar" --manifest "$work/AndroidManifest.xml" \
    --min-sdk-version 29 --target-sdk-version 33 "$work/res.zip"
# Any key will do: a static overlay on /vendor is trusted for its partition, not its signer
keytool -genkeypair -keystore "$work/key.jks" -storepass overlay -keypass overlay -alias overlay \
    -keyalg RSA -keysize 2048 -validity 10000 -dname CN=VineTelephonyOverlay >/dev/null 2>&1
"$bt/apksigner" sign --ks "$work/key.jks" --ks-pass pass:overlay --out "$work/overlay.apk" "$work/unsigned.apk"
for abi in arm64-v8a x86_64; do
    mkdir -p "$root/app/src/main/jniLibs/$abi"
    cp "$work/overlay.apk" "$root/app/src/main/jniLibs/$abi/libvine_telephony_overlay.so"
done
echo "app/src/main/jniLibs/*/libvine_telephony_overlay.so"
