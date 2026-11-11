#!/bin/bash
export PATH="$HOME/fakebin:$PATH"
set -x
cp ~/kbuild/out/.config ~/kbuild/out/.config.prev 2>/dev/null
cp ~/hlk_config.txt ~/kbuild/out/.config
cd ~/kbuild
make -C ~/Code_Opensource/kernel M=$HOME/kbuild ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$HOME/kbuild/out olddefconfig > /tmp/odc.log 2>&1
echo "ODC-RC=$?"
grep -E '^CONFIG_(CFI|CFI_CLANG|CFI_CLANG_SHADOW|HKIP_CFI_HARDEN)=' $HOME/kbuild/out/.config
make -C ~/Code_Opensource/kernel M=$HOME/kbuild ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=$HOME/kbuild/out \
  cfi-flags= cfi-clang-flags= DISABLE_CFI= DISABLE_CFI_CLANG= \
  modules > /tmp/m2.log 2>&1
echo "MOD-RC=$?"
tail -6 /tmp/m2.log
ls -l $HOME/kbuild/hello2.ko
