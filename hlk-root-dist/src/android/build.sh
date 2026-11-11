#!/bin/sh
# Build the HLK Root Guard APK on Linux, without gradle:
#   aapt2 -> javac -> d8 -> addtoapk.py -> zipalign -> apksigner
#
#   sh src/android/build.sh            # build + sign (debug key created once)
#   sh src/android/build.sh install    # build, then adb install -r
#
# Linux-only: the whole project builds on ONE local Linux host.  The toolchain
# lives in ../tools/andtool-linux and is fetched by src/android/fetch_toolchain.sh;
# override with ANDTOOL.  No remote machine is contacted.
#
# Layout:
#   bin/                 deployable artifacts; also the final APK output
#   src/android/         this project: java/, AndroidManifest.xml, debug key
#   src/probe/           probe / root-channel sources and scripts
#   src/sukisu/          shim sources, bootstrap and Termux front-ends
#   build/               transient intermediates, deleted on success
#
# The probe binary is shipped as lib/arm64-v8a/libxprobe.so so the installer
# extracts it under /data/app/<pkg>/lib/arm64; GuardService copies it into its
# code cache before exec.  The official Manager APK is a separate install.
set -eu

case "$(uname -s)" in
    Linux) ;;
    *) echo "!! Linux only: Windows support was dropped (see docs/android.md)" >&2
       exit 1 ;;
esac

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
TC=${ANDTOOL:-"$ROOT/../tools/andtool-linux"}
BT="$TC/bt/android-13"
PLAT="$TC/plat/android-11/android.jar"
# Any Temurin/OpenJDK 17 directory works; fetch_toolchain.sh may pull a newer
# 17.x patch release, so auto-detect instead of hardcoding 17.0.20.1+1.
JDK=${JDK_DIR:-$(find "$TC/jdk" -maxdepth 1 -mindepth 1 -type d -name 'jdk-17*' 2>/dev/null | head -1)}

# An archive produced on Windows loses the executable bit on ELF tools.
# Repair it in place so an offline copy builds without re-running the fetcher
# (a no-op on a native Linux checkout).
[ -x "$BT/aapt2" ] || chmod +x "$BT/aapt2" "$BT/zipalign" 2>/dev/null || true
if [ -n "$JDK" ] && [ ! -x "$JDK/bin/java" ]; then
    chmod +x "$JDK/bin/java" "$JDK/bin/javac" "$JDK/bin/keytool" 2>/dev/null || true
fi

[ -x "$BT/aapt2" ] && [ -x "$JDK/bin/java" ] || {
    echo "!! Linux toolchain missing -> sh src/android/fetch_toolchain.sh" >&2
    exit 1
}

JAVA="$JDK/bin/java"
JAVAC="$JDK/bin/javac"
KEYTOOL="$JDK/bin/keytool"

PROBE=${PROBE:-bin/jit_write_probe_v16}
OUT=build
PKG=com.hlk.rootguard
APK=bin/hlk-root-guard.apk

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" bin

echo "== aapt2 link =="
# aapt2 occasionally fails with "Unable to open android.jar: I/O error" when
# an indexer briefly locks the 21 MB platform jar.  Retrying is enough.
n=0
until "$BT/aapt2" link \
    -o "$OUT/base.apk" \
    -I "$PLAT" \
    --manifest src/android/AndroidManifest.xml \
    --min-sdk-version 28 --target-sdk-version 28 \
    --version-code 14 --version-name 1.4 \
    --no-version-vectors 2>"$OUT/aapt2.err"
do
    n=$((n + 1))
    if [ "$n" -ge 5 ]; then
        echo "!! aapt2 failed after $n tries" >&2
        cat "$OUT/aapt2.err" >&2
        exit 1
    fi
    echo "   aapt2 retry $n"
    sleep 1
done

echo "== javac =="
# android.jar is a stub jar without java.lang.invoke.LambdaMetafactory, so the
# lambda desugaring helper from build-tools has to sit on the classpath.
"$JAVAC" -nowarn -encoding UTF-8 -source 8 -target 8 -bootclasspath "$PLAT" \
    -classpath "$BT/core-lambda-stubs.jar" \
    -d "$OUT/classes" $(find src/android/java -name '*.java')

echo "== d8 =="
"$JAVA" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 \
    --lib "$PLAT" --min-api 28 --release \
    --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')

echo "== package =="
python3 src/android/addtoapk.py "$OUT/base.apk" "$OUT/unsigned.apk" \
    "$OUT/dex/classes.dex" \
    "$PROBE=lib/arm64-v8a/libxprobe.so" \
    "src/probe/rootd.sh=assets/rootd.sh" \
    "bin/rootsrv=assets/rootsrv" \
    "src/probe/r=assets/r" \
    "bin/koffs.txt=assets/koffs.txt" \
    "bin/hlk_ksu_shim9.ko=assets/ksu/hlk_ksu_shim9.ko" \
    "bin/hlk_su=assets/ksu/hlk_su" \
    "src/sukisu/ksu_bootstrap.sh=assets/ksu/ksu_bootstrap.sh" \
    "bin/hlk_sucompat.ko=assets/ksu/hlk_sucompat.ko" \
    "src/sukisu/su.sh=assets/ksu/su.sh" \
    "src/sukisu/termux_su=assets/ksu/termux_su" \
    "src/sukisu/termux_tsu=assets/ksu/termux_tsu" \
    "bin/grant_su=assets/ksu/grant_su" \
    "src/sukisu/hlk_termux_setup.sh=assets/ksu/hlk_termux_setup.sh"

echo "== zipalign =="
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

echo "== sign =="
KS=src/android/debug.keystore
if [ ! -f "$KS" ]; then
    echo "== creating $KS (androiddebugkey / android) =="
    "$KEYTOOL" -genkeypair -v -keystore "$KS" -storepass android \
        -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 \
        -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
"$JAVA" -jar "$BT/lib/apksigner.jar" sign \
    --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --v1-signing-enabled true --v2-signing-enabled true \
    --v4-signing-enabled false \
    --out "$APK" "$OUT/aligned.apk"
"$JAVA" -jar "$BT/lib/apksigner.jar" verify "$APK" >/dev/null && echo "   signature ok"

echo "== clean intermediates =="
rm -rf "$OUT"
rm -f "$APK.idsig"

ls -l "$APK"
md5sum "$APK"

if [ "${1:-}" = install ]; then
    echo "== install =="
    adb install -r -d "$APK"
fi
