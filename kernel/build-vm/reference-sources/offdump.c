#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seccomp.h>
#include <linux/thread_info.h>
#include <stdio.h>

int main(void)
{
	printf("offsetof(task_struct, cred)    = 0x%lx\n",
	       (unsigned long)__builtin_offsetof(struct task_struct, cred));
	printf("offsetof(task_struct, seccomp) = 0x%lx\n",
	       (unsigned long)__builtin_offsetof(struct task_struct, seccomp));
	printf("sizeof(struct seccomp)         = 0x%lx\n",
	       (unsigned long)sizeof(struct seccomp));
	printf("offsetof(thread_info, flags)   = 0x%lx\n",
	       (unsigned long)__builtin_offsetof(struct thread_info, flags));
	return 0;
}
