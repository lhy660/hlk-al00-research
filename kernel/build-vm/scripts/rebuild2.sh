#!/bin/bash
export PATH="$HOME/fakebin:$PATH"
cd ~/kbuild
yes "" | make -C ~/Code_Opensource/kernel M=$HOME/kbuild ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$HOME/kbuild/out oldconfig > /tmp/oc.log 2>&1
echo "OC-RC=$?"
grep -E '^CONFIG_(CFI|CFI_CLANG|CFI_CLANG_SHADOW)=' $HOME/kbuild/out/.config
grep -E 'CONFIG_CFI_CLANG|CONFIG_CFI ' $HOME/kbuild/out/include/generated/autoconf.h
touch hello.c hello2.c
make -C ~/Code_Opensource/kernel M=$HOME/kbuild ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$HOME/kbuild/out \
  cfi-flags= cfi-clang-flags= DISABLE_CFI= DISABLE_CFI_CLANG= \
  modules > /tmp/m3.log 2>&1
echo "MOD-RC=$?"
tail -5 /tmp/m3.log
ls -l $HOME/kbuild/hello2.ko
