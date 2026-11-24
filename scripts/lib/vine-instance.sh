# Sourced by host scripts that drive a rootless guest instance through the
# debug build's engine socket (files/rootless.sock only exists there).
#
#   vine_init [INSTANCE_ID]  -- sets ADB, PKG, LIB, ID; ID defaults to the
#                               single running instance
#   send <cmd> [args...]     -- talk to the engine socket
#   booted                   -- instance $ID is running and booted
#   wait_pm                  -- wait until the guest package manager answers

PKG=com.hexadecinull.vineos.debug
ADB=${ADB:-$(command -v adb || echo ~/Library/Android/sdk/platform-tools/adb)}

die() { echo "ERROR: $*" >&2; exit 1; }
sh_() { "$ADB" shell "$@"; }
send() { sh_ "run-as $PKG $LIB/libvine_helper.so --send files/rootless.sock $*" | tr -d '\r'; }
booted() { send status "$ID" | tr '\n' ' ' | grep -q 'state=running .*boot=1'; }
wait_pm() {
    for _ in $(seq 30); do send exec "$ID" 10000 pm path android | grep -q package: && return; sleep 2; done
    die "guest package manager not ready"
}

vine_init() {
    [ "$(sh_ getprop sys.boot_completed | tr -d '\r')" = 1 ] || die "no adb device"
    LIB=$(sh_ pm path $PKG | head -1 | tr -d '\r' | sed 's/^package://;s/base\.apk$//')lib/arm64
    [ "$LIB" != lib/arm64 ] || die "$PKG not installed"
    ID=${1:-}
    if [ -z "$ID" ]; then
        local running
        running=$(send list | sed -n 's/=running$//p')
        [ -n "$running" ] || die "no running instance; start one in the app first"
        [ "$(echo "$running" | wc -l)" -eq 1 ] || die "several running instances, pass one: $running"
        ID=$running
    fi
    echo "==> instance $ID"
}
