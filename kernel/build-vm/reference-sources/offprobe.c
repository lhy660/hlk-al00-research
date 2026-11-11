#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/stddef.h>

char _o_name[offsetof(struct module, name)];
char _o_init[offsetof(struct module, init)];
char _o_clbase[offsetof(struct module, core_layout.base)];
char _o_ilbase[offsetof(struct module, init_layout.base)];
char _o_exit[offsetof(struct module, exit)];
char _o_sz[sizeof(struct module)];

static int __init op_init(void) { return 0; }
static void __exit op_exit(void) { }
module_init(op_init);
module_exit(op_exit);
MODULE_LICENSE("GPL");
