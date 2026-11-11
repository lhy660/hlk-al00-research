#!/bin/bash
export PATH="$HOME/fakebin:$PATH"
cd ~/kbuild
make -C ~/Code_Opensource/kernel M=$HOME/kbuild ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CLANG_PREBUILTS_PATH=/usr/lib/llvm-21/ O=$HOME/kbuild/out modules > /tmp/hello.log 2>&1
echo "RC=$?"
tail -12 /tmp/hello.log
ls -la ~/kbuild/hello.ko 2>/dev/null
