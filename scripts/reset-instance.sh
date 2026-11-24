#!/bin/bash
# Factory-reset a rootless guest and wait until it is booted again.
#
#   scripts/reset-instance.sh [INSTANCE_ID]
#
# Deleting rootfs.stamp makes the next start re-extract the rootfs and wipe
# data/ and the tawcroot owner store (extract_rootfs in rootless_prep.cpp).
# The restart goes through the app's shortcut intent so the app passes its
# usual start options.
set -euo pipefail
. "$(dirname "$0")/lib/vine-instance.sh"
vine_init "${1:-}"

echo "==> stopping"
send stop "$ID" >/dev/null
for _ in $(seq 30); do send status "$ID" | grep -q '^state=stopped' && break; sleep 1; done
send status "$ID" | grep -q '^state=stopped' || die "instance did not stop"
sh_ "run-as $PKG rm -f files/instances/$ID/rootless/rootfs.stamp"

echo "==> starting (rootfs re-extract + first boot)"
sh_ "am start -n $PKG/com.hexadecinull.vineos.MainActivity -a com.hexadecinull.vineos.action.LAUNCH_APP \
    --es com.hexadecinull.vineos.extra.INSTANCE_ID $ID --es com.hexadecinull.vineos.extra.PACKAGE com.android.launcher3" >/dev/null
for _ in $(seq 120); do booted && break; sleep 5; done
booted || die "instance did not boot in 10 min"
wait_pm
echo "==> done"
