#!/bin/bash
# Install Soda Music (汽水音乐, com.luna.music) and WeChat Input Method
# (微信输入法, com.tencent.wetype) into a running rootless guest.
# Run scripts/reset-instance.sh first for a clean guest.
#
#   scripts/install-qishui.sh [INSTANCE_ID] [QISHUI_APK] [WETYPE_APK]
#
# Without a local APK it is downloaded on the phone (the official links are the
# Android buttons on www.qishui.com/download and z.weixin.qq.com, and may not be
# reachable from the host).
set -euo pipefail
. "$(dirname "$0")/lib/vine-instance.sh"
vine_init "${1:-}"

QISHUI_URL=${QISHUI_URL:-https://ugapk.com/GMg3}
WETYPE_URL=${WETYPE_URL:-https://z.weixin.qq.com/android/download?channel=website}
DATA=files/instances/$ID/rootless/data

booted || die "instance $ID is not booted"
wait_pm

# install_apk NAME URL [LOCAL_APK]
install_apk() {
    local name=$1 url=$2 local_apk=${3:-}
    local tmp=/data/local/tmp/$name.apk guest_apk=/data/local/tmp/$name.apk
    if [ -n "$local_apk" ]; then
        echo "==> [$name] pushing $local_apk"
        "$ADB" push "$local_apk" "$tmp" >/dev/null
    else
        echo "==> [$name] downloading $url on the phone"
        sh_ "curl -fSL --retry 2 -m 600 -o $tmp '$url'" || die "$name: download failed"
    fi
    sh_ "head -c 2 $tmp | grep -q PK" || die "$tmp is not an APK"

    echo "==> [$name] copying into guest $guest_apk"
    sh_ "run-as $PKG sh -c 'mkdir -p $DATA/local/tmp && cat > $DATA/local/tmp/$name.apk' < $tmp"
    sh_ "rm -f $tmp"

    echo "==> [$name] installing (pm install in the guest)"
    local out
    out=$(send exec "$ID" 180000 sh -c "'pm install -r -g -S \$(stat -c %s $guest_apk) < $guest_apk; r=\$?; rm -f $guest_apk; exit \$r'")
    echo "$out"
    echo "$out" | grep -q Success || die "$name: install failed"
}

install_apk qishui "$QISHUI_URL" "${2:-}"
install_apk wetype "$WETYPE_URL" "${3:-}"
echo "==> done"
