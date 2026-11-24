#!/usr/bin/env python3
"""Stages a Pixel 5's /vendor listing for the rootless guest's app view.

    scripts/rootless/stage-vendor-view.py VENDOR_DIR [--out app/src/main/jniLibs/arm64-v8a]

VENDOR_DIR is the vendor partition of redfin's factory image for the
identity's build (TQ3A.230901.001.C2), unpacked, e.g. with
`debugfs -R "rdump / DIR" vendor.img`. Writes <out>/libvine_vendor_view.so,
a text manifest rootless_prep turns into placeholders where the guest's
/vendor lacks the name (install_vendor_view says which, and what apps
then no longer see), one entry per line:

  d PATH                  a directory
  f PATH SIZE MODE        a file: sparse, SIZE bytes of zeros
  t PATH MODE BASE64      a file with this text

Init scripts become their text with every line commented out, so init
parses nothing from them; VINTF fragments and permission files empty but
valid XML, since one that doesn't parse fails the whole device manifest.
Only names, sizes and modes are taken from the image; the libraries
themselves are not.
"""
import argparse
import base64
import os
import stat
import sys

DIRS = ["", "etc/init", "etc/permissions", "etc/vintf/manifest", "bin/hw", "lib64/hw", "lib64", "lib"]
TEXT = {"etc/init": ".rc", "": ".rc"}
EMPTY_XML = {
    "etc/vintf/manifest": b'<manifest version="1.0" type="device">\n</manifest>\n',
    "etc/permissions": b'<?xml version="1.0" encoding="utf-8"?>\n<permissions>\n</permissions>\n',
}


def commented(data: bytes) -> bytes:
    lines = data.decode("utf-8", "replace").split("\n")
    return "\n".join(("#" + l) if l.strip() and not l.lstrip().startswith("#") else l for l in lines).encode()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("vendor")
    ap.add_argument("--out", default="app/src/main/jniLibs/arm64-v8a")
    a = ap.parse_args()
    root = os.path.abspath(a.vendor)
    if not os.path.isfile(os.path.join(root, "build.prop")):
        sys.exit(f"{root}: no build.prop, not a vendor partition")
    out = []
    for d in DIRS:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        prefix = "/vendor" + ("/" + d if d else "")
        for name in sorted(os.listdir(base)):
            p = os.path.join(base, name)
            st = os.lstat(p)
            path = f"{prefix}/{name}"
            mode = stat.S_IMODE(st.st_mode)
            if stat.S_ISDIR(st.st_mode):
                out.append(f"d {path}")
            elif stat.S_ISREG(st.st_mode):
                ext = TEXT.get(d)
                if d in EMPTY_XML and name.endswith(".xml"):
                    out.append(f"t {path} {mode:o} {base64.b64encode(EMPTY_XML[d]).decode()}")
                elif ext and name.endswith(ext):
                    with open(p, "rb") as f:
                        text = base64.b64encode(commented(f.read())).decode()
                    out.append(f"t {path} {mode:o} {text}")
                else:
                    out.append(f"f {path} {st.st_size} {mode:o}")
    os.makedirs(a.out, exist_ok=True)
    dest = os.path.join(a.out, "libvine_vendor_view.so")
    with open(dest, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"{dest}: {len(out)} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
