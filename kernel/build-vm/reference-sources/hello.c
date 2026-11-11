// 阶段1 探针模块：验证签名patch + CRC + vermagic 全链路
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

static int __init hello_init(void)
{
	pr_info("hello-lkm: loaded, vermagic match OK, uid context test\n");
	return 0;
}

static void __exit hello_exit(void)
{
	pr_info("hello-lkm: unloaded cleanly\n");
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HLK stage1 module load chain probe");
