#!/bin/sh
# Fetch the Linux-only build toolchain (no Android Studio / sdkmanager):
#   JDK 17 (Temurin)  -> tools/andtool-linux/jdk/jdk-17*/
#   build-tools 33    -> tools/andtool-linux/bt/android-13  (aapt2, d8, zipalign,
#                                                            apksigner, stubs)
#   platform 30 jar   -> tools/andtool-linux/plat/android-11 (android.jar)
#
# Re-runnable: existing archives and extracted files are kept.
#   sh src/android/fetch_toolchain.sh              # APK toolchain (~300 MB)
#   sh src/android/fetch_toolchain.sh --with-ndk  # + Linux NDK r21e (~1.1 GB)
#   ANDTOOL=/path sh src/android/fetch_toolchain.sh
#
# All URLs are Linux x86_64 binaries; run this on the Linux build host.
set -eu

case "$(uname -s)" in
    Linux) ;;
    *) echo "!! Linux only" >&2; exit 1 ;;
esac

cd "$(dirname "$0")/../.."
TC=${ANDTOOL:-../tools/andtool-linux}
WS=$(cd "$TC/.." && pwd)
DL="$TC/_dl"
mkdir -p "$DL" "$TC/jdk" "$TC/bt" "$TC/plat"

JDK_URL="https://mirrors.tuna.tsinghua.edu.cn/Adoptium/17/jdk/x64/linux/OpenJDK17U-jdk_x64_linux_hotspot_17.0.21_9.tar.gz"
BT_URL="https://dl.google.com/android/repository/build-tools_r33.0.3-linux.zip"
PLAT_URL="https://dl.google.com/android/repository/platform-30_r03.zip"
NDK_URL="https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip"

fetch() { # url file
    if [ -s "$2" ]; then
        echo "   cached $2"
    else
        echo "   downloading $(basename "$2")"
        curl -L --fail --retry 3 -o "$2" "$1"
    fi
}

echo "== build-tools r33.0.3 (Linux) =="
fetch "$BT_URL" "$DL/bt.zip"
unzip -q -o "$DL/bt.zip" -d "$TC/bt"
chmod +x "$TC/bt/android-13/aapt2" "$TC/bt/android-13/zipalign"

echo "== platform 30 (android.jar) =="
fetch "$PLAT_URL" "$DL/plat.zip"
unzip -q -o "$DL/plat.zip" -d "$TC/plat"

echo "== Temurin JDK 17 (Linux x64) =="
if ! find "$TC/jdk" -maxdepth 3 -type f -path '*/bin/java' | grep -q .; then
    fetch "$JDK_URL" "$DL/jdk17.tar.gz"
    tar -xzf "$DL/jdk17.tar.gz" -C "$TC/jdk"
fi
# tarballs normally keep their mode, but be explicit for the tools we exec.
find "$TC/jdk" -type f \( -name java -o -name javac -o -name keytool \) \
    -exec chmod +x {} \; 2>/dev/null || true

if [ "${1:-}" = "--with-ndk" ]; then
    echo "== Android NDK r21e (Linux x64) -- needed to rebuild hlk_su =="
    NDK="$WS/ndk-linux"
    mkdir -p "$NDK"
    fetch "$NDK_URL" "$NDK/_ndk.zip"
    if [ ! -x "$NDK/android-ndk-r21e/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang" ]; then
        unzip -q -o "$NDK/_ndk.zip" -d "$NDK"
    fi
fi

echo "== verify =="
JDK_DIR=$(find "$TC/jdk" -maxdepth 1 -mindepth 1 -type d -name 'jdk-17*' | head -1)
for f in "$TC/bt/android-13/aapt2" "$TC/bt/android-13/zipalign" \
         "$TC/bt/android-13/lib/d8.jar" "$TC/bt/android-13/lib/apksigner.jar" \
         "$TC/bt/android-13/core-lambda-stubs.jar" \
         "$TC/plat/android-11/android.jar" "$JDK_DIR/bin/java" "$JDK_DIR/bin/javac"; do
    [ -e "$f" ] && echo "   OK   $f" || { echo "   MISS $f" >&2; exit 1; }
done
echo "ok. Now: sh src/android/build.sh"
