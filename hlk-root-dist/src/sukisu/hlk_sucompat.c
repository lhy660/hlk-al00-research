// SPDX-License-Identifier: GPL-2.0
/*
 * hlk_sucompat.c -- redirect execve("su") to the HLK root helper.
 *
 * WHY THIS EXISTS
 * ---------------
 * The SukiSU v4.2.1 shim (hlk_ksu_shim9) implements the *driver contract*
 * ([ksu_driver] anon fd + KSU_IOCTL_GRANT_ROOT + the reboot() magic), so an
 * allowlisted app can get root by calling /data/local/tmp/hlk_su.
 * It does NOT implement KernelSU's *sucompat* layer, so a plain
 * Runtime.exec("su") still dies with ENOENT ("No su program found").
 *
 * Real KernelSU/SukiSU solves that by hooking execve: they patch
 * sys_call_table[] and rewrite the syscall into
 * execveat(fd_of_ksud, "", argv, envp, AT_EMPTY_PATH), granting root before
 * the exec.  We cannot do that here: CONFIG_KPROBES=n, CONFIG_FUNCTION_TRACER=n
 * and sys_call_table patching is a hard red line (RO kernel text + CFI).
 *
 * So this module does the *degraded* variant: it stays on the sys_enter
 * tracepoint (the only hook we have) and only rewrites the pathname
 * argument, letting hlk_su do the actual GRANT_ROOT in normal process
 * context.  Effects:
 *   - no syscall table rewrite, no RO text write, no cred games in atomic ctx
 *   - lives only until reboot ([permanent], same as the shim)
 *
 * HOW
 * ---
 * arm64 syscall wrappers read their arguments out of pt_regs at call time, so
 * patching x0/x1 here changes the path the kernel actually execs.
 * The replacement string is written into the caller's own stack, 256 bytes
 * below SP, just like KernelSU's userspace_stack_buffer() -- the exec is
 * about to throw that stack away anyway.
 *
 * Copying to/from user memory inside a tracepoint is the one risky part
 * (RCU / preempt-disabled).  Everything here therefore uses the *_inatomic
 * variants inside pagefault_disable(), and silently gives up on any fault:
 * a missed redirect degrades to the old ENOENT, never to a panic.
 *
 * Huawei tree notes (inherited from hlk_ksu_shim9.c):
 *   - DO NOT include <linux/sched.h>/<linux/cred.h>/<linux/mm.h>/<linux/ptrace.h>
 *     (their prmem instrumentation drags in __start_data_wr / g_prmem_ranges /
 *     init_cred, none of which are exported -> insmod fails with ENOENT).
 *   - current task via "mrs sp_el0"; cred at +0x938, uid at +4.
 *
 * Build: build_sucompat.sh (local Linux Kbuild + Huawei 4.14 tree).
 * Load : insmod hlk_sucompat.ko [sucompat=0] [only_uid=10016] [target=/path]
 *        default target is /data/local/tmp/su.sh (the 3-line PATH launcher)
 * State: cat /proc/hlk_sucompat
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/kallsyms.h>
#include <linux/moduleparam.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

/*
 * CFI pass-through (2026-11-07) -- same rationale as hlk_ksu_shim9.c:
 * the Huawei kernel resolves every cross-module CFI check through the
 * owning module's own page-aligned __cfi_check(); exporting a no-op makes
 * insmod and all tracepoint callbacks into this module pass deterministically
 * instead of racing the probe's kernel-text patch.  Must stay global,
 * page-aligned and named exactly __cfi_check (kernel/cfi.c + kernel/module.c).
 */
void __attribute__((aligned(4096)))
__cfi_check(uint64_t id, void *ptr, void *diag)
{
	(void)id;
	(void)ptr;
	(void)diag;
}

#define HLK_NR_execve		221
#define HLK_NR_execveat		281
/* sucompat 还必须在 access()/stat() 上撒谎，否则"先探测后执行"的 app 会放弃 */
#define HLK_NR_faccessat	48
#define HLK_NR_newfstatat	79
#define HLK_AT_FDCWD		(-100)

/* offset of u64 sp inside struct pt_regs (user_pt_regs: regs[31], sp, pc, pstate) */
#define PT_SP_IDX		31

/* longest entry in su_paths[] is "/debug_ramdisk/su" (19) + NUL */
#define PATHBUF			24
/* room for the replacement path we write into the caller's stack */
#define TGTBUF			96
/*
 * Try several slots below SP, nearest first.  sp-256 alone was too brittle:
 * ART/native stacks often sit near a page boundary (guard) or the target
 * page is not resident, and __copy_to_user_inatomic cannot fault it in
 * from a tracepoint.  A slot 64 bytes below SP is almost always on the
 * same, already-resident stack page that the syscall just used.
 */
#define STACK_TRIES		3
static const unsigned int stack_offs[STACK_TRIES] = { 64, 160, 320 };
#define HLK_PAGE_SIZE		4096UL

/* overridable so a second build can coexist with a loaded one */
#ifndef HLK_PROC_NAME
#define HLK_PROC_NAME		"hlk_sucompat"
#endif

#define TASK_CRED_OFF		0x938
#define CRED_UID_OFF		0x004

static int (*tp_reg)(void *tp, void *fn, void *data);
static int (*tp_unreg)(void *tp, void *fn, void *data);
static void *tp_sys_enter;

static bool sucompat = 1;
module_param(sucompat, bool, 0644);
MODULE_PARM_DESC(sucompat, "1 = redirect execve(\"su\") to target (default 1)");

static char *target = "/data/local/tmp/su.sh";
module_param(target, charp, 0644);
MODULE_PARM_DESC(target, "replacement program (must be executable by the caller)");

static bool fake_stat = 1;
module_param(fake_stat, bool, 0644);
MODULE_PARM_DESC(fake_stat, "1 = also redirect faccessat/newfstatat of su (default 1)");

static int only_uid = -1;
module_param(only_uid, int, 0644);
MODULE_PARM_DESC(only_uid, "-1 = all uids (default); >=0 = redirect only this uid");

static unsigned long n_hits, n_hits_stat, n_skip_uid, n_fault_in, n_fault_out, n_noslot;
static unsigned long n_guard, n_copyfail;
static int n_last_uid;

static const char *const su_paths[] = {
	"su",
	"/system/bin/su",
	"/system/xbin/su",
	"/system/sbin/su",
	"/sbin/su",
	"/su/bin/su",
	"/debug_ramdisk/su",
};

/* ---- self-contained little helpers (no <linux/string.h>, no <linux/sched.h>) */

static inline void *hlk_cur_task(void)
{
	void *p;

	__asm__ __volatile__("mrs %0, sp_el0" : "=r"(p));
	return p;
}

static inline int hlk_cur_uid(void)
{
	void *t = hlk_cur_task();
	void *cred;

	if (!t)
		return -1;
	cred = *(void **)((char *)t + TASK_CRED_OFF);
	if (!cred)
		return -1;
	return *(int *)((char *)cred + CRED_UID_OFF);
}

static int hlk_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int hlk_strlen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

/*
 * pagefault_disable() keeps a fault from trying to sleep inside the
 * tracepoint: the copy just reports "not copied" and we bail out.
 */
static int ucopy_from(void *dst, const void __user *src, unsigned long n)
{
	int ret;

	pagefault_disable();
	ret = __copy_from_user_inatomic(dst, src, n);
	pagefault_enable();
	return ret;
}

static int ucopy_to(void __user *dst, const void *src, unsigned long n)
{
	int ret;

	pagefault_disable();
	ret = __copy_to_user_inatomic(dst, src, n);
	pagefault_enable();
	return ret;
}

/*
 * Put @s on the caller's stack just below SP and return the user address,
 * or 0 when we do not trust the location.
 */
static unsigned long stack_slot(unsigned long *r, const char *s, unsigned long len)
{
	unsigned long sp = r[PT_SP_IDX], slot, page_off;
	int k;

	if (!sp)
		return 0;
	page_off = sp & (HLK_PAGE_SIZE - 1);
	for (k = 0; k < STACK_TRIES; k++) {
		unsigned int off = stack_offs[k];

		/* &~0xF can push the slot another 15 bytes down; leave headroom */
		if (page_off < off + 16)
			continue;
		slot = (sp - off) & ~0xFUL;
		if (!ucopy_to((void __user *)slot, s, len))
			return slot;
		n_copyfail++;
	}
	n_guard++;
	return 0;
}

static void sucompat_probe(void *data, void *regs_v, long id)
{
	unsigned long *r = (unsigned long *)regs_v;
	const char __user *p;
	char buf[PATHBUF];
	char c;
	unsigned long len, slot;
	int i, uid, arg = -1, is_exec = 0;

	if (unlikely(!sucompat))
		return;

	if (id == HLK_NR_execve) {
		arg = 0;
		is_exec = 1;
	} else if (id == HLK_NR_execveat) {
		/* only the plain "execveat(AT_FDCWD, path, argv, envp, 0)" form */
		if ((int)r[0] != HLK_AT_FDCWD || r[4] != 0)
			return;
		arg = 1;
		is_exec = 1;
	} else if (id == HLK_NR_faccessat || id == HLK_NR_newfstatat) {
		/*
		 * KernelSU redirects these to its own su too: without it a caller
		 * that probes "does su exist?" gives up before execve.
		 * Cheap filter first: stat/faccessat are far more frequent than
		 * execve, so peek one byte and skip anything that cannot be "su"
		 * or a path to it.
		 */
		if (!fake_stat || (int)r[0] != HLK_AT_FDCWD)
			return;
		arg = 1;
		if (ucopy_from(&c, (const char __user *)r[arg], 1))
			return;
		if (c != 's' && c != '/')
			return;
	} else {
		return;
	}

	p = (const char __user *)r[arg];
	if (!p)
		return;

	/* cheap prefix filter: every entry in su_paths[] starts with 's' or '/' */
	if (ucopy_from(&c, p, 1))
		return;
	if (c != 's' && c != '/')
		return;

	/* fault in the pathname; anything odd -> leave the syscall alone */
	if (ucopy_from(buf, p, sizeof(buf))) {
		n_fault_in++;
		return;
	}
	buf[sizeof(buf) - 1] = '\0';

	for (i = 0; i < (int)ARRAY_SIZE(su_paths); i++)
		if (hlk_streq(buf, su_paths[i]))
			break;
	if (i == (int)ARRAY_SIZE(su_paths))
		return;

	uid = hlk_cur_uid();
	if (only_uid >= 0 && uid != only_uid) {
		n_skip_uid++;
		return;
	}

	len = hlk_strlen(target) + 1;
	if (len > TGTBUF) {
		n_noslot++;
		return;
	}

	slot = stack_slot(r, target, len);
	if (!slot) {
		n_fault_out++;
		return;
	}

	r[arg] = slot;
	if (is_exec)
		n_hits++;
	else
		n_hits_stat++;
	n_last_uid = uid;
}

/* ---- /proc/hlk_sucompat ---- */
static int sucompat_show(struct seq_file *m, void *v)
{
	seq_printf(m,
		   "hlk_sucompat: enabled=%d target=%s only_uid=%d fake_stat=%d\n"
		   "counters: hits=%lu hits_stat=%lu skip_uid=%lu fault_in=%lu fault_out=%lu noslot=%lu\n"
		   "slot: guard=%lu copyfail=%lu\n"
		   "last_uid=%d tp=%p\n",
		   sucompat ? 1 : 0, target, only_uid, fake_stat ? 1 : 0,
		   n_hits, n_hits_stat, n_skip_uid, n_fault_in, n_fault_out, n_noslot,
		   n_guard, n_copyfail, n_last_uid, tp_sys_enter);
	return 0;
}

static int sucompat_open(struct inode *inode, struct file *file)
{
	return single_open(file, sucompat_show, NULL);
}

static const struct file_operations sucompat_fops = {
	.owner   = THIS_MODULE,
	.open    = sucompat_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static struct proc_dir_entry *g_pde;

static int __init hlk_sucompat_init(void)
{
	tp_reg = (void *)kallsyms_lookup_name("tracepoint_probe_register");
	tp_unreg = (void *)kallsyms_lookup_name("tracepoint_probe_unregister");
	tp_sys_enter = (void *)kallsyms_lookup_name("__tracepoint_sys_enter");

	if (!tp_reg || !tp_unreg || !tp_sys_enter) {
		pr_err("hlk_sucompat: symbol resolution failed\n");
		return -ENOENT;
	}

	g_pde = proc_create(HLK_PROC_NAME, 0666, NULL, &sucompat_fops);
	if (!g_pde)
		return -ENOMEM;

	if (tp_reg(tp_sys_enter, (void *)sucompat_probe, NULL)) {
		pr_err("hlk_sucompat: tracepoint_probe_register failed\n");
		remove_proc_entry(HLK_PROC_NAME, NULL);
		return -EINVAL;
	}

	pr_info("hlk_sucompat: ready target=%s only_uid=%d enabled=%d fake_stat=%d\n",
		target, only_uid, sucompat ? 1 : 0, fake_stat ? 1 : 0);
	return 0;
}

/* No module_exit on purpose -> [permanent]; rmmod trips vendor CFI. */
module_init(hlk_sucompat_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HLK sucompat: execve(\"su\") -> /data/local/tmp/hlk_su via pt_regs");
