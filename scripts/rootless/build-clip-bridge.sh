#!/bin/sh
# Builds libvine_clip_bridge.so into app/src/main/jniLibs/<abi>/: the dex jar rootless_prep installs as the guest's
# /system/framework/vine-clip.jar (clipbridge/src/dev/vine/clip/ClipBridge.java). Gradle runs it before every build.
set -e
root="$(cd "$(dirname "$0")/../.." && pwd)"
here="$root/scripts/rootless/clipbridge"
sdk="${ANDROID_HOME:-$(sed -n 's/^sdk.dir=//p' "$root/local.properties" 2>/dev/null)}"
bt="$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)"
jar="$(ls -d "$sdk"/platforms/android-*/android.jar | sort -V | tail -1)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/classes" "$work/out"
javac -nowarn --release 11 -cp "$jar" -d "$work/classes" $(find "$here/src" -name '*.java')
"$bt/d8" --release --min-api 33 --lib "$jar" --output "$work/out" $(find "$work/classes" -name '*.class')
(cd "$work/out" && jar -c -0 -M -f "$work/clip.jar" classes.dex)
for abi in arm64-v8a x86_64; do
    mkdir -p "$root/app/src/main/jniLibs/$abi"
    cp "$work/clip.jar" "$root/app/src/main/jniLibs/$abi/libvine_clip_bridge.so"
done
echo "app/src/main/jniLibs/*/libvine_clip_bridge.so"
