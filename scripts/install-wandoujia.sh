#!/bin/bash
# Install Wandoujia (豌豆荚) into a running rootless guest.
# Run scripts/reset-instance.sh first for a clean guest.
#
#   scripts/install-wandoujia.sh [INSTANCE_ID] [LOCAL_APK]
#
# Without LOCAL_APK the APK is downloaded on the phone (the official link
# is the one on www.wandoujia.com and may not be reachable from the host).
set -euo pipefail
. "$(dirname "$0")/lib/vine-instance.sh"
vine_init "${1:-}"

URL=${WDJ_URL:-https://ucan.25pp.com/Wandoujia_wandoujia_sem_default.apk}
TMP=/data/local/tmp/wandoujia.apk
GUEST_APK=/data/local/tmp/wandoujia.apk
DATA=files/instances/$ID/rootless/data

booted || die "instance $ID is not booted"
wait_pm

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
