#!/bin/bash
set -e
H=~/Code_Opensource/kernel/include/linux/module.h
cp -n $H ${H}.orig
python3 - "$H" <<'PY'
import sys,re
p=sys.argv[1]
s=open(p).read()
old="""#ifdef CONFIG_CFI_CLANG
#ifdef CONFIG_HKIP_CFI_HARDEN
	struct safe_cfi_area {
		cfi_check_fn cfi_check;
		struct module *owner;
	} *safe_cfi_area;
#else
	cfi_check_fn cfi_check;
#endif
#endif
"""
new="""	/* HKIP local build: the device kernel always has this slot
	 * (CONFIG_CFI_CLANG=y).  Our gcc build cannot enable CFI, so force
	 * the field so struct module matches the running kernel byte for
	 * byte -- mod->init is 0x178 there, not 0x170. */
	void *safe_cfi_area;
"""
assert old in s, "pattern not found"
s=s.replace(old,new,1)
open(p,'w').write(s)
print("patched")
PY
grep -n "safe_cfi_area" $H
