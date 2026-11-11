// 阶段1b：验证模块 init 真的在内核里执行（不依赖 printk）
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static int hlk_show(struct seq_file *m, void *v)
{
	seq_printf(m, "hlk-hello2 alive jiffies=%lu\n", jiffies);
	return 0;
}

static int hlk_open(struct inode *i, struct file *f)
{
	return single_open(f, hlk_show, NULL);
}

static const struct file_operations hlk_fops = {
	.owner   = THIS_MODULE,
	.open    = hlk_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static struct proc_dir_entry *hlk_pde;

static int __init hello2_init(void)
{
	hlk_pde = proc_create("hlkhello", 0444, NULL, &hlk_fops);
	pr_info("hello2: init pde=%lu\n", (unsigned long)hlk_pde);
	return 0;
}

static void __exit hello2_exit(void)
{
	if (hlk_pde)
		remove_proc_entry("hlkhello", NULL);
	pr_info("hello2: exit\n");
}

module_init(hello2_init);
module_exit(hello2_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HLK stage1b: prove module init executes");
