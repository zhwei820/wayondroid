#!/bin/bash
# Builds the environment probe, runs it inside the running rootless guest as an ordinary app, prints its
# report (lines tagged "<<MARK" name an emulator, Waydroid, LineageOS or the host) and uninstalls it.
# Runs on the dev machine: needs adb, the Android SDK (build-tools, android-33) and a JDK.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
bt="$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)"
jar="$sdk/platforms/android-33/android.jar"
jdk="${JAVA_HOME:-/Applications/Android Studio.app/Contents/jbr/Contents/Home}"
adb="${ADB:-$sdk/platform-tools/adb}"
host=com.hexadecinull.vineos.debug
pkg=dev.probe.env
w="$(mktemp -d)"
trap 'rm -rf "$w"' EXIT

cat > "$w/AndroidManifest.xml" <<EOF
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="$pkg">
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.READ_PHONE_STATE" />
    <uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
    <uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
    <uses-permission android:name="android.permission.CHANGE_WIFI_STATE" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
    <uses-permission android:name="android.permission.QUERY_ALL_PACKAGES" />
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
    <application android:label="probe"><activity android:name=".Main" android:exported="true" /></application>
</manifest>
EOF
mkdir -p "$w/classes" "$w/dex"
"$jdk/bin/javac" -nowarn --release 11 -cp "$jar" -d "$w/classes" $(find "$here/src" -name '*.java') 2>&1 | grep -v '^Note:' || true
"$bt/d8" --min-api 29 --lib "$jar" --output "$w/dex" $(find "$w/classes" -name '*.class')
"$bt/aapt2" link -o "$w/u.apk" -I "$jar" --manifest "$w/AndroidManifest.xml" --min-sdk-version 29 --target-sdk-version 33
(cd "$w/dex" && zip -q "$w/u.apk" classes.dex)
"$jdk/bin/keytool" -genkeypair -keystore "$w/k.jks" -storepass probe1 -keypass probe1 -alias p -keyalg RSA \
    -keysize 2048 -validity 1000 -dname CN=probe >/dev/null 2>&1
"$bt/apksigner" sign --ks "$w/k.jks" --ks-pass pass:probe1 --out "$w/probe.apk" "$w/u.apk" 2>/dev/null

# The guest, through the engine's debug socket (debug builds)
helper="$("$adb" shell "cat /proc/\$(pidof -s libvine_helper.so)/cmdline" | tr '\0' '\n' | head -1)"
id="$("$adb" shell "run-as $host ls files/instances" | tr -d '\r' | head -1)"
gx() { "$adb" shell "run-as $host $helper --send files/rootless.sock exec $id 60000 sh -c $(printf %q "$1")" | grep -v linkerconfig; }
data="files/instances/$id/rootless/data"
"$adb" exec-in "run-as $host sh -c 'cat > $data/local/tmp/probe.apk'" < "$w/probe.apk"
# pm install PATH: PackageManager's SELinux check refuses a file the app (its host uid) wrote; stdin is fine
gx "f=/data/local/tmp/probe.apk; cat \$f | pm install -r -S \$(stat -c %s \$f) >/dev/null; rm \$f
    for p in READ_PHONE_STATE ACCESS_FINE_LOCATION BLUETOOTH_CONNECT; do pm grant $pkg android.permission.\$p; done
    rm -f /data/data/$pkg/files/report.txt; am start -W -n $pkg/.Main >/dev/null
    for i in \$(seq 1 30); do [ -s /data/data/$pkg/files/report.txt ] && break; sleep 1; done
    cat /data/data/$pkg/files/report.txt; pm uninstall $pkg >/dev/null" | grep -v '^exit='
