#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

/* 1) 自提 RLIMIT_CORE（pipe 模式要求 limit>0） 2) 触发 SIGSEGV dump */
int main(void)
{
	struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
	if (setrlimit(RLIMIT_CORE, &rl) != 0)
		perror("setrlimit");
	printf("rlimit set, about to segfault\n");
	fflush(stdout);
	volatile int *p = (volatile int *)0;
	*p = 0xdeadbeef;
	return 0;
}
