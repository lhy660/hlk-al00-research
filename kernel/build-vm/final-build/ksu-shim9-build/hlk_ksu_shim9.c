/*
 * hlk_ksu_shim9.c -- SukiSU Ultra v4.2.1 (40994-5) shim, v3:
 * add Superuser app authorization on top of the v2 supercall shim.
 *
 * What v3 adds over hlk_ksu_shim5.c:
 *   1. GRANT_ROOT now also switches the caller's SELinux domain to
 *      u:r:shell:s0 by writing /proc/self/attr/current through the normal
 *      kernel VFS path (filp_open + kernel_write).  This uses SELinux's own
 *      setprocattr implementation, so NO struct cred / task_security_struct
 *      offsets are guessed and NO read-only kernel memory is touched.
 *      Without this, root shells stay u:r:kernel:s0 and cannot reach
 *      servicemanager/PackageManager, so RootService (and thus the Superuser
 *      app list) never starts.
 *   2. A small in-module allowlist with real GET/SET_APP_PROFILE and
 *      NEW_GET_ALLOW_LIST ioctls, so the Manager can grant/revoke root.
 *   3. The sys_enter tracepoint probe also auto-escalates processes whose uid
 *      is on the allowlist (KSU's execve-hook equivalent for this device).
 *   4. /proc injection is restricted to uid 0, closing the v2 hole where any
 *      process could install itself a driver fd.
 *
 * Deployment note: this is loaded as a THIRD module because v1/v2 are
 * [permanent].  Before loading, v2's reboot probe must be silenced by setting
 * its allow_uid to a non-manager value (write a shell-owned pid into
 * /proc/hlksu_fd); otherwise two probes would both install [ksu_driver] fds.
 * Then point ksu_fdd.sh at /proc/hlksu_fd9 and restart the Manager so it holds
 * exactly one v3 fd.
 *
 * Conventions inherited from ksu-lkm/hlk_su.c and v1/v2:
 *   - DO NOT include <linux/cred.h> / <linux/sched.h> (Huawei prmem headers
 *     drag in unexported symbols).
 *   - All non-exported helpers resolved via kallsyms_lookup_name().
 *   - current via "mrs sp_el0"; cred at +0x938, uid at +4.
 *   - NO module_exit -> [permanent], accidental rmmod returns -EBUSY instead
 *     of taking the CFI slow path that reboots this device.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/kallsyms.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * CFI pass-through (2026-11-07).
 *
 * The Huawei 4.14 kernel is built with CONFIG_CFI_CLANG + CFI_CLANG_SHADOW.
 * Any indirect call from kernel text into THIS module that fails the
 * caller-side type check is routed to __cfi_slowpath_diag() ->
 * cfi_slowpath_handler() (kernel/cfi.c), which resolves the owning module's
 * own __cfi_check() through the CFI shadow.  The module loader registers
 * that hook unconditionally for every module (kernel/module.c: cfi_init()
 * -> mod_find_symname(mod, "__cfi_check") -> cfi_harden_module_init()).
 * A gcc-built module without the symbol leaves its shadow pages invalid,
 * the lookup returns NULL and handle_cfi_failure() raises
 * VENDOR_EXCEPTION(VENDOR_MODID_AP_S_CFI) -- a full SoC reset that made
 * insmod probabilistic and was the real cause of the 1500 s gate.
 *
 * Fix: export a page-aligned no-op __cfi_check.  The kernel then hands the
 * CFI verdict for *all* cross-module calls into this module to us, and an
 * empty body always passes.  No kernel text / RO memory is touched.
 * add_module_to_shadow() BUGs unless the symbol is page-aligned, hence the
 * aligned(4096).  Do NOT make this static, do NOT rename it.
 */
void __attribute__((aligned(4096)))
__cfi_check(uint64_t id, void *ptr, void *diag)
{
	(void)id;
	(void)ptr;
	(void)diag;
}

#define TASK_CRED_OFF		0x938
#define CRED_UID_OFF		0x004

/*
 * Huawei 4.14 cred layout, read back from init_cred in kimage.bin by the
 * v16 probe (RESEARCH_NOTES C-16).  Kept as raw offsets because this module
 * deliberately does not include <linux/cred.h> (Huawei prmem headers drag in
 * unexported symbols).
 *
 *   0x04..0x24  uid,gid,suid,sgid,euid,egid,fsuid,fsgid   (8 x u32)
 *   0x24        securebits
 *   0x28..0x48  cap_inheritable / permitted / effective / bset (4 x u32[2])
 *   0x48        cap_ambient
 *   0x50..0x78  jit_keyring + session/process/thread/request_key_auth
 *   0x78        security (SELinux SID blob)
 *
 * GRANT_ROOT only writes 0x04..0x48.  Everything from 0x50 on -- the FBE
 * keyrings, the LSM blob, user_ns and group_info -- is left exactly as the
 * kernel built it.  That is what keeps /data writable after the uid switch.
 */
#define CRED_UIDS_LEN		0x020
#define CRED_CAP_OFF		0x028
#define CRED_CAP_WORDS		8	/* 4 kernel_cap_t = 8 u32 */
#define CAP_FULL_LO		0xffffffffU
#define CAP_FULL_HI		0x0000007fU	/* 0x0000007fffffffff */

/* Must match SukiSU v4.2.1 (40994-5) Manager APK and its ksud. */
#define KSU_VERSION_CODE	40994
#define KSU_UAPI_VERSION	5
#define KSU_FEATURE_MAX		0

#define KSU_FLAG_LKM		(1U << 0)
#define KSU_FLAG_MANAGER	(1U << 1)
#define KSU_FLAG_LATE_LOAD	(1U << 2)
#define KSU_FLAGS		(KSU_FLAG_LKM | KSU_FLAG_MANAGER | KSU_FLAG_LATE_LOAD)

#define PROC_NODE		"hlksu_fd9"
#define DRIVER_FD_NAME		"[ksu_driver]"
#define KSU_MAGIC1		0xDEADBEEFU
#define KSU_MAGIC2		0xCAFEBABEU
#define HLK_NR_reboot		142

#define SHELL_CTX		"u:r:shell:s0"
#define HLK_O_WRONLY		1

/* uapi/app_profile.h @ v4.2.1 -- byte-accurate copy */
#define KSU_APP_PROFILE_VER	4
#define KSU_MAX_PACKAGE_NAME	256
#define KSU_MAX_GROUPS		32
#define KSU_SELINUX_DOMAIN	64
#define FLAG_KSU_NO_NEW_PRIVS	(1ULL << 0)

struct root_profile {
	__s32 uid;
	__s32 gid;
	__u32 groups_count;
	__s32 groups[KSU_MAX_GROUPS];
	struct {
		__u64 effective;
		__u64 permitted;
		__u64 inheritable;
	} capabilities;
	char selinux_domain[KSU_SELINUX_DOMAIN];
	__s32 namespaces;
	__u64 flags;
};

struct non_root_profile {
	bool umount_modules;
};

struct app_profile {
	__u32 version;
	char key[KSU_MAX_PACKAGE_NAME];
	__s32 curr_uid;
	bool allow_su;
	union {
		struct {
			bool use_default;
			char template_name[KSU_MAX_PACKAGE_NAME];
			struct root_profile profile;
		} rp_config;
		struct {
			bool use_default;
			struct non_root_profile profile;
		} nrp_config;
	};
};

struct ksu_get_app_profile_cmd {
	struct app_profile profile;
};

struct ksu_set_app_profile_cmd {
	struct app_profile profile;
};

/* uapi/supercall.h @ v4.2.1 -- minimal subset */
struct ksu_get_info_cmd {
	__u32 version;
	__u32 flags;
	__u32 features;
	__u32 uapi_version;
};

struct ksu_new_get_allow_list_cmd {
	__u16 count;
	__u16 total_count;
	__u32 uids[0];
};

struct ksu_check_safemode_cmd {
	__u8 in_safe_mode;
};

struct ksu_get_feature_cmd {
	__u32 feature_id;
	__u64 value;
	__u8 supported;
};

#define KSU_IOCTL_GRANT_ROOT		_IOC(_IOC_NONE, 'K', 1, 0)
#define KSU_IOCTL_GET_INFO		_IOR('K', 2, struct ksu_get_info_cmd)
#define KSU_IOCTL_GET_INFO_LEGACY	_IOC(_IOC_READ, 'K', 2, 0)
#define KSU_IOCTL_REPORT_EVENT		_IOC(_IOC_WRITE, 'K', 3, 0)
#define KSU_IOCTL_CHECK_SAFEMODE	_IOC(_IOC_READ, 'K', 5, 0)
#define KSU_IOCTL_NEW_GET_ALLOW_LIST	_IOWR('K', 6, struct ksu_new_get_allow_list_cmd)
#define KSU_IOCTL_UID_SHOULD_UMOUNT	_IOC(_IOC_READ | _IOC_WRITE, 'K', 9, 0)
#define KSU_IOCTL_GET_APP_PROFILE	_IOC(_IOC_READ | _IOC_WRITE, 'K', 11, 0)
#define KSU_IOCTL_SET_APP_PROFILE	_IOC(_IOC_WRITE, 'K', 12, 0)
#define KSU_IOCTL_GET_FEATURE		_IOC(_IOC_READ | _IOC_WRITE, 'K', 13, 0)
#define KSU_IOCTL_GET_FULL_VERSION	_IOC(_IOC_READ, 'K', 100, 0)
#define KSU_IOCTL_HOOK_TYPE		_IOC(_IOC_READ, 'K', 101, 0)

/* ---- runtime-resolved kernel helpers ---- */
struct shim_cb_head {
	struct shim_cb_head *next;
	void (*func)(struct shim_cb_head *work);
};

typedef int  (*tw_add_fn)(void *task, struct shim_cb_head *work, bool notify);
typedef void *(*find_task_fn)(int nr);
typedef int  (*aigfd_fn)(const char *name, const struct file_operations *fops,
			 void *priv, int flags);
typedef void *(*pkc_fn)(void *);
typedef void *(*pc_fn)(void);		/* prepare_creds, no argument */
typedef int  (*cc_fn)(void *);
typedef int  (*tp_reg_fn)(void *tp, void *probe, void *data);
typedef void *(*filp_open_fn)(const char *, int, unsigned short);
typedef long (*kernel_write_fn)(void *, const void *, unsigned long, long long *);
typedef int  (*filp_close_fn)(void *, void *);

static tw_add_fn		g_tw_add;
static find_task_fn		g_find_task;
static aigfd_fn			g_aigfd;
static pkc_fn			g_pkc;
static pc_fn			g_pc;
static cc_fn			g_cc;
static tp_reg_fn		g_tp_reg;
static tp_reg_fn		g_tp_unreg;	/* same 3-arg signature, int return */
static void			*g_tp_sys_enter;
static void			*g_old_probe;	/* v3's sys_enter_probe, if found */
static int			g_old_unreg_rc = -ENOENT;
static filp_open_fn		g_filp_open;
static kernel_write_fn		g_kernel_write;
static filp_close_fn		g_filp_close;
static int			g_tp_rc = -ENOENT;

static unsigned long		g_grants;
static unsigned long		g_fails;
static unsigned long		g_getinfo;
static unsigned long		g_ioctl_other;
static unsigned long		g_tp_hits;
static unsigned long		g_tw_fails;
static unsigned long		g_auto_roots;
static unsigned long		g_setcon_ok;
static unsigned long		g_setcon_fail;
static int			g_last_fd = -1;
static int			g_req_pid;
static int			g_inject_rc;
static int			g_mgr_uid = 10164;	/* updated by fd injection */
static volatile int		g_cb_busy;

static struct shim_cb_head g_cb;

/* ---- allowlist ---- */
#define MAX_ALLOW	64

struct allow_entry {
	volatile int used;
	int uid;
	int allow_su;
	char key[KSU_MAX_PACKAGE_NAME];
};

static struct allow_entry g_allow[MAX_ALLOW];
static volatile int g_allow_su_count;

/*
 * Processes we already queued a driver-fd install for, keyed by TGID.
 *
 * v3..v5 keyed this by task_struct pointer, i.e. per THREAD.  A Compose
 * Manager plus a granted app easily exceed the 128-slot ring; evicted live
 * threads were reinstalled on their very next syscall, and the device hit
 * 1946 [ksu_driver] fds in ONE app process within seconds (RLIMIT_NOFILE
 * is 32768, but the churn is unbounded).
 *
 * Now dedup per PROCESS using the exported __task_pid_nr_ns() with
 * __PIDTYPE_TGID (== 4 on this Huawei 4.14 tree, NOT the mainline 1).
 * A stale tgid only suppresses one install; a new process gets a new tgid.
 * CRITICAL: only the Manager uid is ever targeted, never zygote (uid 0) --
 * task_work_add on zygote rebooted this device during v3 deployment.
 */
#define MAX_SEEN 256
#define HLK_PIDTYPE_TGID 4
typedef int (*pid_nr_fn)(void *task, int type, void *ns);
static pid_nr_fn g_pid_nr;
static int g_seen[MAX_SEEN];
static volatile int g_seen_head;
static unsigned long g_auto_fds;

static int seen_has(int tgid)
{
	int i;

	if (tgid <= 0)
		return 1;		/* no tgid -> do not install */
	for (i = 0; i < MAX_SEEN; i++)
		if (g_seen[i] == tgid)
			return 1;
	return 0;
}

static void seen_add(int tgid)
{
	int i = __sync_fetch_and_add(&g_seen_head, 1) % MAX_SEEN;

	g_seen[i] = tgid;
}

static struct allow_entry *allow_find(int uid)
{
	int i;

	for (i = 0; i < MAX_ALLOW; i++)
		if (g_allow[i].used && g_allow[i].uid == uid)
			return &g_allow[i];
	return NULL;
}

static int allow_has_su(int uid)
{
	struct allow_entry *e = allow_find(uid);

	return e && e->allow_su;
}

static int allow_set(int uid, const char *key, int allow_su)
{
	struct allow_entry *e = allow_find(uid);
	int i;
	int old;

	if (!e) {
		for (i = 0; i < MAX_ALLOW; i++) {
			if (__sync_bool_compare_and_swap(&g_allow[i].used, 0, 1)) {
				e = &g_allow[i];
				break;
			}
		}
	}
	if (!e)
		return -ENOSPC;

	old = e->allow_su;
	e->uid = uid;
	e->allow_su = allow_su;
	memset(e->key, 0, sizeof(e->key));
	if (key)
		strncpy(e->key, key, sizeof(e->key) - 1);
	if (old != allow_su) {
		if (allow_su)
			__sync_add_and_fetch(&g_allow_su_count, 1);
		else
			__sync_sub_and_fetch(&g_allow_su_count, 1);
	}
	return 0;
}

static inline void *cur_task(void)
{
	void *p;

	__asm__ __volatile__("mrs %0, sp_el0" : "=r"(p));
	return p;
}

static inline void *task_cred(void *t)
{
	return t ? *(void **)((char *)t + TASK_CRED_OFF) : NULL;
}

static inline int cred_uid(void *cred)
{
	return cred ? *(int *)((char *)cred + CRED_UID_OFF) : -1;
}

static inline int cur_uid(void)
{
	return cred_uid(task_cred(cur_task()));
}

static inline int hlk_is_err(void *p)
{
	return (unsigned long)p >= (unsigned long)(-4095L);
}

/*
 * Switch current's SELinux domain through the kernel's own procfs attribute
 * interface.  selinux_setprocattr() validates the context and installs a new
 * cred for us -- no manual sid/offset poking, so a mistake here can at worst
 * fail the write, never corrupt kernel memory.
 */
static int set_self_ctx(void)
{
	void *f;
	long long pos = 0;
	long n;

	if (!g_filp_open || !g_kernel_write || !g_filp_close)
		return -ENOENT;
	f = g_filp_open("/proc/self/attr/current", HLK_O_WRONLY, 0);
	if (!f || hlk_is_err(f))
		return -ENOENT;
	n = g_kernel_write(f, SHELL_CTX, sizeof(SHELL_CTX) - 1, &pos);
	g_filp_close(f, NULL);
	return (n > 0) ? 0 : -EIO;
}

/* ---- ioctl dispatch ---- */
static long shim_unlocked_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	int uid = cur_uid();

	switch (cmd) {
	case KSU_IOCTL_GET_INFO: {
		struct ksu_get_info_cmd info = {
			.version      = KSU_VERSION_CODE,
			.flags        = KSU_FLAGS,
			.features     = KSU_FEATURE_MAX,
			.uapi_version = KSU_UAPI_VERSION,
		};

		g_getinfo++;
		if (copy_to_user(uarg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case KSU_IOCTL_GET_INFO_LEGACY: {
		struct {
			__u32 version;
			__u32 flags;
			__u32 features;
		} legacy = {
			.version  = KSU_VERSION_CODE,
			.flags    = KSU_FLAGS,
			.features = KSU_FEATURE_MAX,
		};

		g_getinfo++;
		if (copy_to_user(uarg, &legacy, sizeof(legacy)))
			return -EFAULT;
		return 0;
	}
	case KSU_IOCTL_GRANT_ROOT: {
		void *new = NULL;
		char *c;
		unsigned int i;
		int used_pc = 0;

		if (uid != 0 && uid != 2000 && uid != g_mgr_uid &&
		    !allow_has_su(uid)) {
			g_fails++;
			return -EPERM;
		}
		if (!g_cc || (!g_pc && !g_pkc)) {
			g_fails++;
			return -ENOENT;
		}

		/*
		 * ENOKEY fix (2026-11-09).
		 *
		 * prepare_kernel_cred(NULL) clones init_cred but unconditionally
		 * NULLs all four keyring pointers (kernel/cred.c).  The resulting
		 * root shell is uid 0 with full caps yet loses the FBE session
		 * keyring: reading/appending existing /data files works, but
		 * creating a file or a second-level directory fails with
		 * "Required key not available" -- the root shell cannot create
		 * files or directories, which breaks ordinary root workflows.
		 *
		 * prepare_creds() instead clones current's cred and key_get()s
		 * every keyring, and runs security_prepare_creds() to copy the
		 * SELinux blob.  We then overwrite only the uid/gid and
		 * capability words before commit_creds().  Under
		 * CONFIG_HKIP_PROTECT_CRED commit_creds() copies the finished
		 * cred into a read-only prmem page, so editing it here (before
		 * the commit) is exactly the right moment.
		 *
		 * Fallback: if prepare_creds is unavailable or fails, keep the
		 * old prepare_kernel_cred(NULL) behaviour rather than lose root.
		 */
		if (g_pc) {
			new = g_pc();
			if (new)
				used_pc = 1;
		}
		if (!new && g_pkc)
			new = g_pkc(NULL);
		if (!new) {
			g_fails++;
			return -ENOMEM;
		}

		if (used_pc) {
			c = (char *)new;
			/*
			 * Layout guard.  prepare_creds() must have copied our
			 * uid to CRED_UID_OFF.  If Huawei ever changes the
			 * layout, refuse instead of writing at a wrong offset.
			 * Committing this untouched clone of our own cred is
			 * harmless and releases the allocation cleanly.
			 */
			if (*(u32 *)(c + CRED_UID_OFF) != (u32)uid) {
				pr_err("hlk_ksu_shim9: cred uid offset "
				       "mismatch, refusing grant\n");
				(void)g_cc(new);
				g_fails++;
				return -EINVAL;
			}
			for (i = 0; i < CRED_UIDS_LEN / 4; i++)
				((u32 *)(c + CRED_UID_OFF))[i] = 0;
			for (i = 0; i < CRED_CAP_WORDS; i++)
				((u32 *)(c + CRED_CAP_OFF))[i] =
					(i & 1) ? CAP_FULL_HI : CAP_FULL_LO;
		}

		if (g_cc(new)) {
			g_fails++;
			return -EIO;
		}
		/* selinux_setprocattr() prepares creds from current again, so
		 * the keyrings survive the domain switch too. */
		if (set_self_ctx() == 0)
			g_setcon_ok++;
		else
			g_setcon_fail++;
		g_grants++;
		return 0;
	}
	case KSU_IOCTL_SET_APP_PROFILE: {
		struct ksu_set_app_profile_cmd set_cmd;

		if (uid != 0 && uid != 2000 && uid != g_mgr_uid)
			return -EPERM;
		if (copy_from_user(&set_cmd, uarg, sizeof(set_cmd)))
			return -EFAULT;
		return allow_set(set_cmd.profile.curr_uid,
				 set_cmd.profile.key,
				 set_cmd.profile.allow_su ? 1 : 0);
	}
	case KSU_IOCTL_GET_APP_PROFILE: {
		struct ksu_get_app_profile_cmd get_cmd;
		struct allow_entry *e;

		if (copy_from_user(&get_cmd, uarg, sizeof(get_cmd)))
			return -EFAULT;
		e = allow_find(get_cmd.profile.curr_uid);
		if (!e)
			return -ENOENT;		/* JNI falls back to default profile */

		get_cmd.profile.allow_su = e->allow_su ? true : false;
		get_cmd.profile.version = KSU_APP_PROFILE_VER;
		if (get_cmd.profile.allow_su) {
			struct root_profile *rp =
				&get_cmd.profile.rp_config.profile;

			get_cmd.profile.rp_config.use_default = true;
			memset(get_cmd.profile.rp_config.template_name, 0,
			       sizeof(get_cmd.profile.rp_config.template_name));
			memset(rp, 0, sizeof(*rp));
			rp->uid = 0;
			rp->gid = 0;
			rp->capabilities.effective = ~0ULL;
			strncpy(rp->selinux_domain, SHELL_CTX,
				sizeof(rp->selinux_domain) - 1);
			rp->flags = FLAG_KSU_NO_NEW_PRIVS;
		} else {
			get_cmd.profile.nrp_config.use_default = true;
			get_cmd.profile.nrp_config.profile.umount_modules = true;
		}
		if (copy_to_user(uarg, &get_cmd, sizeof(get_cmd)))
			return -EFAULT;
		return 0;
	}
	case KSU_IOCTL_NEW_GET_ALLOW_LIST: {
		struct ksu_new_get_allow_list_cmd hdr = { 0 };
		__u32 su[MAX_ALLOW];
		int total = 0, i, want;

		/*
		 * CRITICAL: struct ksu_new_get_allow_list_cmd is
		 *   __u16 count; __u16 total_count; __u32 uids[0];
		 * sizeof == 4 and offsetof(uids) == 4.  The Manager JNI
		 * getSuperuserCount() declares it as a 4-byte STACK object.
		 * Writing 8 bytes (two __u32) overflows the native stack and
		 * corrupts the return address -> SIGSEGV 0x7800000000.
		 * Copy back exactly sizeof(hdr) bytes and put uids at +4.
		 */
		if (copy_from_user(&hdr, uarg, sizeof(hdr)))
			return -EFAULT;
		for (i = 0; i < MAX_ALLOW; i++)
			if (g_allow[i].used && g_allow[i].allow_su)
				su[total++] = (__u32)g_allow[i].uid;
		want = hdr.count < total ? hdr.count : total;
		hdr.count = (__u16)want;
		hdr.total_count = (__u16)total;
		if (copy_to_user(uarg, &hdr, sizeof(hdr)))
			return -EFAULT;
		if (want > 0 &&
		    copy_to_user((char __user *)uarg + sizeof(hdr),
				 su, sizeof(__u32) * want))
			return -EFAULT;
		return 0;
	}
	case KSU_IOCTL_CHECK_SAFEMODE: {
		struct ksu_check_safemode_cmd sm = { .in_safe_mode = 0 };

		if (copy_to_user(uarg, &sm, sizeof(sm)))
			return -EFAULT;
		return 0;
	}
	case KSU_IOCTL_GET_FEATURE: {
		struct ksu_get_feature_cmd ft = { .value = 0, .supported = 0 };

		if (copy_from_user(&ft.feature_id, uarg, sizeof(__u32)))
			return -EFAULT;
		ft.value = 0;
		ft.supported = 0;
		if (copy_to_user(uarg, &ft, sizeof(ft)))
			return -EFAULT;
		return 0;
	}
	case KSU_IOCTL_GET_FULL_VERSION: {
		char buf[255] = { 0 };

		strncpy(buf, "v4.2.1-40994-hlk-shim9", sizeof(buf) - 1);
		if (copy_to_user(uarg, buf, sizeof(buf)))
			return -EFAULT;
		return 0;
	}
	case KSU_IOCTL_HOOK_TYPE: {
		char buf[32] = { 0 };

		strncpy(buf, "Tracepoint", sizeof(buf) - 1);
		if (copy_to_user(uarg, buf, sizeof(buf)))
			return -EFAULT;
		return 0;
	}
	case KSU_IOCTL_REPORT_EVENT:
	case KSU_IOCTL_UID_SHOULD_UMOUNT:
		return 0;
	default:
		g_ioctl_other++;
		return -ENOTTY;
	}
}

static const struct file_operations shim_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = shim_unlocked_ioctl,
	.compat_ioctl   = shim_unlocked_ioctl,
};

/* ---- fixed-size task_work pool (no slab/spinlock headers) ---- */
#define TW_SLOTS	48

struct fd_tw {
	struct shim_cb_head cb;
	int __user *outp;
};

static struct fd_tw g_tws[TW_SLOTS];
static volatile int g_used[TW_SLOTS];

static struct fd_tw *tw_alloc(void)
{
	int i;

	for (i = 0; i < TW_SLOTS; i++) {
		if (__sync_bool_compare_and_swap(&g_used[i], 0, 1)) {
			struct fd_tw *tw = &g_tws[i];

			memset(tw, 0, sizeof(*tw));
			return tw;
		}
	}
	return NULL;
}

static void tw_free(struct fd_tw *tw)
{
	int i = tw - g_tws;

	if (i >= 0 && i < TW_SLOTS)
		__sync_bool_compare_and_swap(&g_used[i], 1, 0);
}

static void reboot_fd_cb(struct shim_cb_head *work)
{
	struct fd_tw *tw = (struct fd_tw *)work;
	int fd = -1;

	if (g_aigfd)
		fd = g_aigfd(DRIVER_FD_NAME, &shim_fops, NULL, 0);
	if (fd >= 0 && tw->outp) {
		if (copy_to_user(tw->outp, &fd, sizeof(fd)))
			pr_warn("hlk_ksu_shim9: failed to return fd\n");
	}
	g_last_fd = fd;
	tw_free(tw);
}

/* Manual injection callback for the Manager process. */
static void install_fd_cb(struct shim_cb_head *work)
{
	int fd = -1;

	if (g_aigfd)
		fd = g_aigfd(DRIVER_FD_NAME, &shim_fops, NULL, 0);
	g_last_fd = fd;
	g_cb_busy = 0;
}

/* Automatic injection: no outp to write, just install the fd. */
static void auto_install_fd_cb(struct shim_cb_head *work)
{
	struct fd_tw *tw = (struct fd_tw *)work;
	int fd = -1;

	if (g_aigfd)
		fd = g_aigfd(DRIVER_FD_NAME, &shim_fops, NULL, 0);
	g_last_fd = fd;
	tw_free(tw);
}

/*
 * sys_enter probe: serve the reboot magic (upstream ksud behaviour) and
 * auto-escalate allowlisted uids.  No uid filter on the magic path: holding a
 * driver fd is harmless because GRANT_ROOT / SET_APP_PROFILE are gated above.
 */
static void shim9_sys_enter_probe(void *data, void *regs, long id)
{
	unsigned long *r = (unsigned long *)regs;
	struct fd_tw *tw;
	void *t;
	int uid;

	uid = cur_uid();
	t = cur_task();

	/*
	 * Install ONE driver fd per Manager PROCESS (dedup by tgid, not thread).
	 * The fd exists before HomeViewModel.buildState() reads GET_INFO.
	 *
	 * Allowlisted apps are deliberately NOT auto-installed: the hlk_su
	 * helper obtains a fresh fd on demand via reboot(MAGIC1, MAGIC2, &fd),
	 * which the reboot-magic branch below serves.  v5 auto-installed into
	 * every granted app thread and produced an unbounded fd leak.
	 *
	 * DO NOT auto-escalate here.  v3/v4 granted root on the first syscall,
	 * which turned a freshly forked app into uid 0 BEFORE it attached to
	 * system_server -> "attachApplicationLocked ... belongs to another
	 * existing app" and the app was killed.  Root is now granted ONLY when
	 * the app explicitly issues KSU_IOCTL_GRANT_ROOT, gated by
	 * allow_has_su() in shim_unlocked_ioctl().
	 *
	 * NEVER target uid 0 (zygote) -- task_work_add there rebooted the device.
	 */
	if (g_mgr_uid > 0 && uid == g_mgr_uid && g_pid_nr) {
		int tgid = g_pid_nr(t, HLK_PIDTYPE_TGID, NULL);

		if (!seen_has(tgid)) {
			tw = tw_alloc();
			if (tw) {
				tw->cb.next = NULL;
				tw->cb.func = auto_install_fd_cb;
				if (g_tw_add(t, &tw->cb, true) == 0) {
					seen_add(tgid);
					g_auto_fds++;
				} else {
					tw_free(tw);
				}
			}
		}
	}

	if (id != HLK_NR_reboot)
		return;
	if ((__u32)r[0] != KSU_MAGIC1 || (__u32)r[1] != KSU_MAGIC2)
		return;
	if (!r[3] || !g_tw_add)
		return;

	tw = tw_alloc();
	if (!tw) {
		g_tw_fails++;
		return;
	}
	tw->outp = (int __user *)r[3];
	tw->cb.next = NULL;
	tw->cb.func = reboot_fd_cb;
	if (g_tw_add(cur_task(), &tw->cb, true)) {
		tw_free(tw);
		g_tw_fails++;
		return;
	}
	g_tp_hits++;
}

/* ---- /proc/hlksu_fd9 ---- */
static int fd_show(struct seq_file *m, void *v)
{
	seq_printf(m,
		   "hlk_ksu_shim9: version=%d uapi=%d flags=0x%x\n"
		   "tracepoint: reg_rc=%d tp=%p hits=%lu tw_fails=%lu old_probe=%p old_unreg=%d\n"
		   "setcon: ok=%lu fail=%lu auto_roots=%lu auto_fds=%lu\n"
		   "mgr_uid=%d allow_su=%d sizeof(app_profile)=%zu\n"
		   "last: req_pid=%d fd=%d rc=%d busy=%d\n"
		   "counters: getinfo=%lu grants=%lu fails=%lu other=%lu\n"
		   "symbols: tw_add=%p aigfd=%p pkc=%p pc=%p cc=%p fop=%p kw=%p fc=%p\n"
		   "fbe_keyring: prepare_creds=%p mode=%s\n"
		   "caller_uid=%d\n",
		   KSU_VERSION_CODE, KSU_UAPI_VERSION, KSU_FLAGS,
		   g_tp_rc, g_tp_sys_enter, g_tp_hits, g_tw_fails,
		   g_old_probe, g_old_unreg_rc,
		   g_setcon_ok, g_setcon_fail, g_auto_roots, g_auto_fds,
		   g_mgr_uid, g_allow_su_count, sizeof(struct app_profile),
		   g_req_pid, g_last_fd, g_inject_rc, g_cb_busy,
		   g_getinfo, g_grants, g_fails, g_ioctl_other,
		   g_tw_add, g_aigfd, g_pkc, g_pc, g_cc,
		   g_filp_open, g_kernel_write, g_filp_close,
		   g_pc, g_pc ? "preserved" : "legacy(ENOKEY)", cur_uid());
	return 0;
}

static int fd_open(struct inode *i, struct file *f)
{
	return single_open(f, fd_show, NULL);
}

static ssize_t fd_write(struct file *f, const char __user *ubuf,
			size_t n, loff_t *off)
{
	char buf[32] = { 0 };
	long pid = 0;
	void *task;
	int rc;

	/* Only the root channel may use the injection interface. */
	if (cur_uid() != 0)
		return -EACCES;
	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';

	/* "u <uid>" sets the manager uid without needing a live pid. */
	if (buf[0] == 'u' && (buf[1] == ' ' || buf[1] == ':')) {
		long v;

		if (kstrtol(buf + 2, 10, &v))
			return -EINVAL;
		g_mgr_uid = (int)v;
		return n;
	}

	if (kstrtol(buf, 10, &pid))
		return -EINVAL;
	if (!g_find_task || !g_tw_add || !g_aigfd)
		return -ENOENT;
	if (g_cb_busy)
		return -EBUSY;
	task = g_find_task((int)pid);
	if (!task)
		return -ESRCH;

	g_mgr_uid = cred_uid(task_cred(task));
	g_req_pid = (int)pid;
	g_cb_busy = 1;
	g_cb.next = NULL;
	g_cb.func = install_fd_cb;
	rc = g_tw_add(task, &g_cb, true);
	g_inject_rc = rc;
	if (rc) {
		g_cb_busy = 0;
		return rc;
	}
	return n;
}

static const struct file_operations fd_fops = {
	.owner   = THIS_MODULE,
	.open    = fd_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write   = fd_write,
};

static struct proc_dir_entry *g_pde;

static int __init hlk_ksu_shim9_init(void)
{
	unsigned long (*kln)(const char *);

	kln = (void *)kallsyms_lookup_name("kallsyms_lookup_name");
	if (!kln)
		return -ENOENT;

	g_tw_add      = (void *)kln("task_work_add");
	g_find_task   = (void *)kln("find_task_by_vpid");
	g_aigfd       = (void *)kln("anon_inode_getfd");
	g_pkc         = (void *)kln("prepare_kernel_cred");
	g_pc          = (void *)kln("prepare_creds");
	g_cc          = (void *)kln("commit_creds");
	g_tp_reg      = (void *)kln("tracepoint_probe_register");
	g_tp_unreg    = (void *)kln("tracepoint_probe_unregister");
	g_tp_sys_enter = (void *)kln("__tracepoint_sys_enter");
	/*
	 * Retire the previous shim (v5 / hlk_ksu_shim8) whose probe is still
	 * registered: it is [permanent] and its per-thread fd injection leaks
	 * fds without bound (1946 in one app process within seconds).
	 * mod_find_symname() does not filter STB_LOCAL, so a static module
	 * symbol is resolvable via kallsyms_lookup_name().
	 */
	g_old_probe   = (void *)kln("shim8_sys_enter_probe");
	g_pid_nr      = (void *)kln("__task_pid_nr_ns");
	g_filp_open   = (void *)kln("filp_open");
	g_kernel_write = (void *)kln("kernel_write");
	g_filp_close  = (void *)kln("filp_close");

	if (!g_tw_add || !g_find_task || !g_aigfd || !g_pkc || !g_cc ||
	    !g_pid_nr || !g_filp_open || !g_kernel_write || !g_filp_close) {
		pr_err("hlk_ksu_shim9: symbol resolution incomplete\n");
		return -ENOENT;
	}
	/* Optional: without it GRANT_ROOT falls back to the old ENOKEY-prone
	 * prepare_kernel_cred(NULL) path instead of failing to load. */
	pr_info("hlk_ksu_shim9: prepare_creds=%p FBE keyring %s\n",
		g_pc, g_pc ? "preserved" : "LOST (legacy fallback)");

	g_pde = proc_create(PROC_NODE, 0666, NULL, &fd_fops);
	if (!g_pde)
		return -ENOMEM;

	if (g_tp_unreg && g_tp_sys_enter && g_old_probe) {
		g_old_unreg_rc = g_tp_unreg(g_tp_sys_enter, g_old_probe, NULL);
		pr_info("hlk_ksu_shim9: retired v3 probe %p rc=%d\n",
			g_old_probe, g_old_unreg_rc);
	}

	if (g_tp_reg && g_tp_sys_enter) {
		g_tp_rc = g_tp_reg(g_tp_sys_enter, (void *)shim9_sys_enter_probe, NULL);
		pr_info("hlk_ksu_shim9: sys_enter probe rc=%d\n", g_tp_rc);
	}

	pr_info("hlk_ksu_shim9: ready mgr_uid=%d app_profile=%zu\n",
		g_mgr_uid, sizeof(struct app_profile));
	return 0;
}

/* No module_exit on purpose -> [permanent]. */
module_init(hlk_ksu_shim9_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SukiSU v4.2.1 shim v6: tgid-deduped fd install, request-based root");
