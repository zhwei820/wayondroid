#!/bin/bash
# One-shot install of Wandoujia (豌豆荚) into a running rootless guest.
#
#   scripts/install-wandoujia.sh [INSTANCE_ID] [LOCAL_APK]
#
# Needs the debug build (the engine's debug socket files/rootless.sock only
# exists there). INSTANCE_ID defaults to the single running instance.
# Without LOCAL_APK the APK is downloaded on the phone (the official link
# is the one on www.wandoujia.com and may not be reachable from the host).
set -euo pipefail

PKG=com.hexadecinull.vineos.debug
URL=${WDJ_URL:-https://ucan.25pp.com/Wandoujia_wandoujia_sem_default.apk}
ADB=${ADB:-$(command -v adb || echo ~/Library/Android/sdk/platform-tools/adb)}
TMP=/data/local/tmp/wandoujia.apk
GUEST_APK=/data/local/tmp/wandoujia.apk

die() { echo "ERROR: $*" >&2; exit 1; }
sh_() { "$ADB" shell "$@"; }

[ "$(sh_ getprop sys.boot_completed | tr -d '\r')" = 1 ] || die "no adb device"
LIB=$(sh_ pm path $PKG | head -1 | tr -d '\r' | sed 's/^package://;s/base\.apk$//')lib/arm64
[ "$LIB" != lib/arm64 ] || die "$PKG not installed"
send() { sh_ "run-as $PKG $LIB/libvine_helper.so --send files/rootless.sock $*" | tr -d '\r'; }

ID=${1:-}
if [ -z "$ID" ]; then
    running=$(send list | sed -n 's/=running$//p')
    [ -n "$running" ] || die "no running instance; start one in the app first"
    [ "$(echo "$running" | wc -l)" -eq 1 ] || die "several running instances, pass one: $running"
    ID=$running
fi
DATA=files/instances/$ID/rootless/data
echo "==> instance $ID"

if [ -n "${2:-}" ]; then
    echo "==> pushing $2"
    "$ADB" push "$2" $TMP >/dev/null
else
    echo "==> downloading $URL on the phone"
    sh_ "curl -fSL --retry 2 -m 300 -o $TMP '$URL'" || die "download failed"
fi
sh_ "head -c 2 $TMP | grep -q PK" || die "$TMP is not an APK"

echo "==> copying into guest $GUEST_APK"
sh_ "run-as $PKG sh -c 'mkdir -p $DATA/local/tmp && cat > $DATA/local/tmp/wandoujia.apk' < $TMP"
sh_ "rm -f $TMP"

echo "==> installing (pm install in the guest)"
out=$(send exec "$ID" 180000 sh -c "'pm install -r -g -S \$(stat -c %s $GUEST_APK) < $GUEST_APK; r=\$?; rm -f $GUEST_APK; exit \$r'")
echo "$out"
echo "$out" | grep -q Success || die "install failed"
echo "==> done"
