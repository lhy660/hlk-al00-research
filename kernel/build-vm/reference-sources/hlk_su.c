// 阶段2a：最小授权模块（raw 版）
//
// 为什么不用 <linux/cred.h>：它 include 了 <linux/hisi/prmem.h>，华为把 cred
// 放进了自己的受保护内存框架，头文件里的内联会引用 init_cred / init_task /
// __start_data_wr / g_prmem_ranges 这些**没有导出**的符号，模块加载直接
// "Unknown symbol" 挂掉。
//
// 所以这里全部走原始偏移 + kallsyms：
//   内核里 sys_getuid 的代码是  mrs x8, sp_el0 ; ldr x8,[x8,#0x938] ; ldr w8,[x8,#4]
//   -> task_struct.cred  在 0x938（当前构建固定）
//   -> cred.uid          在 +0x04
//   提权用内核导出的 prepare_kernel_cred(NULL) + commit_creds()，只作用于 current。
//
// 用法：echo go > /proc/hlksu     （调用者自己变成 uid 0 / 全 caps / kernel sid）

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/kallsyms.h>

#define TASK_CRED_OFF	0x938	/* offsetof(struct task_struct, cred) */
#define CRED_UID_OFF	0x004	/* offsetof(struct cred, uid) */

#define ALLOW_UID	2000	/* adb shell */

static unsigned long g_grants;
static unsigned long g_fails;
static int g_last_pid = -1;

static inline void *cur_task(void)
{
	void *p;

	__asm__ __volatile__("mrs %0, sp_el0" : "=r"(p));
	return p;
}

static inline int cur_uid(void)
{
	void *t = cur_task();
	void *cred = *(void **)((char *)t + TASK_CRED_OFF);

	if (!cred)
		return -1;
	return *(int *)((char *)cred + CRED_UID_OFF);
}

static int su_show(struct seq_file *m, void *v)
{
	seq_printf(m, "hlk_su: caller_uid=%d last_pid=%d grants=%lu fails=%lu\n",
		   cur_uid(), g_last_pid, g_grants, g_fails);
	return 0;
}

static int su_open(struct inode *i, struct file *f)
{
	return single_open(f, su_show, NULL);
}

static ssize_t su_write(struct file *f, const char __user *ubuf,
			size_t n, loff_t *off)
{
	unsigned long (*kln)(const char *);
	void *(*pkc)(void *);
	int (*cc)(void *);
	int uid = cur_uid();

	if (uid != ALLOW_UID) {
		g_fails++;
		return -EPERM;
	}

	kln = (void *)kallsyms_lookup_name("kallsyms_lookup_name");
	pkc = (void *)kln("prepare_kernel_cred");
	cc  = (void *)kln("commit_creds");
	if (!pkc || !cc) {
		g_fails++;
		return -ENOENT;
	}

	{
		void *new = pkc(NULL);

		if (!new) {
			g_fails++;
			return -ENOMEM;
		}
		if (cc(new)) {
			g_fails++;
			return -EIO;
		}
	}

	g_last_pid = (int)*(int *)((char *)cur_task() + 0x2c);	/* task->pid */
	g_grants++;
	pr_info("hlk_su: pid %d uid %d -> %d, caps full\n",
		g_last_pid, uid, cur_uid());
	return n;
}

static const struct file_operations su_fops = {
	.owner   = THIS_MODULE,
	.open    = su_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write   = su_write,
};

static struct proc_dir_entry *g_pde;

static int __init hlk_su_init(void)
{
	g_pde = proc_create("hlksu", 0666, NULL, &su_fops);
	if (!g_pde) {
		pr_err("hlk_su: proc_create failed\n");
		return -ENOMEM;
	}
	pr_info("hlk_su: ready (allow_uid=%d)\n", ALLOW_UID);
	return 0;
}

static void __exit hlk_su_exit(void)
{
	if (g_pde)
		remove_proc_entry("hlksu", NULL);
	pr_info("hlk_su: exit grants=%lu\n", g_grants);
}

module_init(hlk_su_init);
module_exit(hlk_su_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HLK stage2a: raw-offset cred escalation for a whitelisted uid");
