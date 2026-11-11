#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>

/* 最小 ptrace 探针：attach 目标一次，立即 detach，报告成功/失败 */
int main(int argc, char **argv)
{
	int pid = atoi(argv[1]);
	long r = ptrace(PTRACE_ATTACH, pid, 0, 0);
	printf("attach rc=%ld errno=%d(%s)\n", r, errno, strerror(errno));
	if (r == 0) {
		int st;
		waitpid(pid, &st, __WALL);
		r = ptrace(PTRACE_DETACH, pid, 0, 0);
		printf("detach rc=%ld errno=%d\n", r, errno);
		printf("PTRACE-TEST-OK\n");
	} else {
		printf("PTRACE-TEST-FAIL\n");
	}
	return 0;
}
