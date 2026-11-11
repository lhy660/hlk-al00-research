#!/bin/bash
export PATH="$HOME/fakebin:$PATH"
cd ~/kbuild
yes "" | make -C ~/Code_Opensource/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$HOME/kbuild/out oldconfig > /tmp/oc2.log 2>&1
echo "OC-RC=$?"
tail -3 /tmp/oc2.log
grep -cE 'CONFIG_CFI' $HOME/kbuild/out/include/generated/autoconf.h
grep -E 'CONFIG_CFI_CLANG|CONFIG_CFI ' $HOME/kbuild/out/include/generated/autoconf.h
touch hello.c hello2.c
make -C ~/Code_Opensource/kernel M=$HOME/kbuild ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$HOME/kbuild/out \
  cfi-flags= cfi-clang-flags= DISABLE_CFI= DISABLE_CFI_CLANG= \
  modules > /tmp/m4.log 2>&1
echo "MOD-RC=$?"
tail -3 /tmp/m4.log
ls -l $HOME/kbuild/hello2.ko
