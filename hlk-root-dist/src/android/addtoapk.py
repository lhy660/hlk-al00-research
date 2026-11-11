#!/usr/bin/env python3
"""Add classes.dex (and optionally the native probe) to an APK produced by
aapt2 link.  Python's zipfile is used instead of `zip` because Git Bash on
Windows does not always ship one, and we need a deterministic order.

  python addtoapk.py base.apk out.apk classes.dex [lib/arm64-v8a/libxprobe.so]
"""
import sys
import zipfile


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    src, dst, dex = sys.argv[1], sys.argv[2], sys.argv[3]
    extra = sys.argv[4:]

    with zipfile.ZipFile(src) as zin:
        names = zin.namelist()
        if "AndroidManifest.xml" not in names:
            sys.exit("base apk has no AndroidManifest.xml")
        with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
            for it in zin.infolist():
                zout.writestr(it, zin.read(it.filename))
            zout.write(dex, "classes.dex")
            for path in extra:
                if "=" in path:
                    disk, arc = path.split("=", 1)
                else:
                    disk, arc = path, path
                zout.write(disk, arc)
    print("wrote %s (%d entries)" % (dst, len(zipfile.ZipFile(dst).namelist())))


if __name__ == "__main__":
    main()
