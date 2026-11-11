#!/bin/sh
# Rebuild the two arm64 helpers used by deploy.sh / verify.sh.
# Run from this directory.  Requires NDK r21e in the workspace.
set -e
CC=../../tools/ndk/android-ndk-r21e/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang
"$CC" -static -O2 -o as_uid as_uid.c
"$CC" -static -O2 -o grant_su grant_su.c
ls -l as_uid grant_su
