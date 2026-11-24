#!/bin/sh
# Builds libvine_pm_patch.so into app/src/main/jniLibs/<abi>/: a jar (pmpatch/src with dexlib2 and
# guava, and each pmpatch/hook class as <class>.dex) that rootless_prep runs on the device's ART to
# patch the guest's services.jar, service-connectivity.jar and service-wifi.jar (pmpatch/src/dev/vine/pmpatch/Main.java).
# Gradle runs it before every build.
#
# Needs the Android SDK (build-tools with d8, a platform's android.jar), a JDK and, once, the
# network for the two pinned jars.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
here="$root/scripts/rootless/pmpatch"
sdk="${ANDROID_HOME:-$(sed -n 's/^sdk.dir=//p' "$root/local.properties" 2>/dev/null)}"
bt="$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)"
jar="$(ls -d "$sdk"/platforms/android-*/android.jar | sort -V | tail -1)"
cache="$root/build/pmpatch-deps"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fetch() { # URL SHA256
    f="$cache/$(basename "$1")"
    if [ ! -f "$f" ] || ! echo "$2  $f" | sha256sum -c - >/dev/null 2>&1; then
        mkdir -p "$cache"
        curl -sSfL -o "$f.part" "$1"
        echo "$2  $f.part" | sha256sum -c - >/dev/null || { echo "checksum mismatch: $1" >&2; exit 1; }
        mv "$f.part" "$f"
    fi
    echo "$f"
}
dexlib2="$(fetch https://dl.google.com/android/maven2/com/android/tools/smali/smali-dexlib2/3.0.9/smali-dexlib2-3.0.9.jar \
    8b547506f62f91d74b70f4beb4989b99e1ff61fc50b2525e9796ca35f0a351dc)"
guava="$(fetch https://repo1.maven.org/maven2/com/google/guava/guava/31.1-android/guava-31.1-android.jar \
    32ac2ed709d96d278b5d2e3e5cea178fa4939939c525fb647532f013308db309)"

mkdir -p "$work/hook" "$work/main" "$work/out"
javac -nowarn --release 11 -cp "$jar" -d "$work/hook" \
    $(find "$here/hook" "$here/stubs" -name '*.java')
# One dex per hook, <class>.dex, each with its own copy of the shared VineWifiInfo (hooks go into
# jars with separate class loaders); the stubs stand for the guest's own classes and stay out
shared="$(find "$work/hook" -name 'VineWifiInfo*.class')"
for src in $(find "$here/hook" -name 'Vine*.java' ! -name 'VineWifiInfo.java'); do
    h="$(basename "$src" .java)"
    mkdir -p "$work/dex-$h"
    "$bt/d8" --min-api 33 --lib "$jar" --classpath "$work/hook" --output "$work/dex-$h" \
        $(find "$work/hook" -name "$h.class" -o -name "$h\$*.class") $shared
    cp "$work/dex-$h/classes.dex" "$work/out/$h.dex"
done
javac -nowarn --release 11 -cp "$dexlib2:$guava" -d "$work/main" $(find "$here/src" -name '*.java')
"$bt/d8" --release --min-api 29 --lib "$jar" --output "$work/out" \
    $(find "$work/main" -name '*.class') "$dexlib2" "$guava" 2>&1 | grep -v '^Warning' >&2 || true
(cd "$work/out" && jar -c -0 -M -f "$work/pmpatch.jar" *.dex)
for abi in arm64-v8a x86_64; do
    mkdir -p "$root/app/src/main/jniLibs/$abi"
    cp "$work/pmpatch.jar" "$root/app/src/main/jniLibs/$abi/libvine_pm_patch.so"
done
echo "app/src/main/jniLibs/*/libvine_pm_patch.so"
