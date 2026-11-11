// 阶段2a：最小授权模块（cred 替换版）
//
// 走的弯路，记下来免得重踩：
//   - CONFIG_KPROBES / CONFIG_FUNCTION_TRACER 都没编 -> kprobe/ftrace 都不可用
//   - capability_hooks 在 __lsm_ro_after_init 段，而 arm64 的 set_memory_rw()
//     只接受 vmalloc 区（find_vm_area + VM_ALLOC），内核镜像地址一律 -EINVAL，
//     所以「改 LSM 钩子表指针」需要自己做 PTE 手术，先放一放。
//
// 先用最直接的方式交付「授权」：把目标进程的 cred 换成内核 cred
// （uid 0 + 全 capability + init_cred 的 SELinux sid）。
//
//   echo <pid> > /proc/hlkgrant   # 授权
//   cat /proc/hlkgrant            # 看统计

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/cred.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/kallsyms.h>

static int g_last_pid = -1;
static unsigned long g_grants;
static unsigned long g_fails;

static int grant_show(struct seq_file *m, void *v)
{
	seq_printf(m, "hlk_grant: last_pid=%d grants=%lu fails=%lu\n",
		   g_last_pid, g_grants, g_fails);
	return 0;
}

static int grant_open(struct inode *i, struct file *f)
{
	return single_open(f, grant_show, NULL);
}

static ssize_t grant_write(struct file *f, const char __user *ubuf,
			   size_t n, loff_t *off)
{
	struct task_struct *t;
	struct cred *new;
	struct pid *p;
	char buf[24];
	int pid = 0;

	if (n == 0 || n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = 0;
	if (kstrtoint(buf, 10, &pid) || pid <= 0)
		return -EINVAL;

	/* 只允许一次授权一个 pid，够验证用 */
	p = find_get_pid(pid);
	if (!p) {
		g_fails++;
		return -ESRCH;
	}

	rcu_read_lock();
	t = pid_task(p, PIDTYPE_PID);
	if (!t) {
		rcu_read_unlock();
		put_pid(p);
		g_fails++;
		return -ESRCH;
	}

	new = prepare_kernel_cred(NULL);
	if (!new) {
		rcu_read_unlock();
		put_pid(p);
		g_fails++;
		return -ENOMEM;
	}

	/* 目标进程直接拿到内核 cred：uid=0、caps 全开、sid=kernel */
	rcu_assign_pointer(t->real_cred, new);
	rcu_assign_pointer(t->cred, new);
	rcu_read_unlock();
	put_pid(p);

	g_last_pid = pid;
	g_grants++;
	pr_info("hlk_grant: pid %d granted kernel cred\n", pid);
	return n;
}

static const struct file_operations grant_fops = {
	.owner   = THIS_MODULE,
	.open    = grant_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write   = grant_write,
};

static struct proc_dir_entry *g_pde;

static int __init hlk_grant_init(void)
{
	g_pde = proc_create("hlkgrant", 0644, NULL, &grant_fops);
	if (!g_pde) {
		pr_err("hlk_grant: proc_create failed\n");
		return -ENOMEM;
	}
	pr_info("hlk_grant: ready, write a pid to /proc/hlkgrant\n");
	return 0;
}

static void __exit hlk_grant_exit(void)
{
	if (g_pde)
		remove_proc_entry("hlkgrant", NULL);
	pr_info("hlk_grant: exit grants=%lu\n", g_grants);
}

module_init(hlk_grant_init);
module_exit(hlk_grant_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HLK stage2a: grant kernel cred to a chosen pid");
