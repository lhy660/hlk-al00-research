#!/bin/bash
set -e
H=~/Code_Opensource/kernel/arch/arm64/include/asm/module.h
cp -n $H ${H}.orig 2>/dev/null || true
python3 - "$H" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="""	/* for CONFIG_DYNAMIC_FTRACE */
	struct plt_entry 	*ftrace_trampoline;
"""
new="""	/* for CONFIG_DYNAMIC_FTRACE: the shipped kernel has DYNAMIC_FTRACE off,
	 * so this slot does not exist there -- keeping it costs 8 bytes and
	 * shifts mod->exit (0x308 on device vs 0x310 here). */
#ifdef CONFIG_DYNAMIC_FTRACE
	struct plt_entry 	*ftrace_trampoline;
#endif
"""
assert old in s
s=s.replace(old,new,1); open(p,'w').write(s); print("patched")
PY
