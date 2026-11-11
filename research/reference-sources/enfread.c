#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>

int main(void)
{
	cpu_set_t cs;
	char b[32] = {0};
	char buf[4096];
	int fd, of, n, m;

	CPU_ZERO(&cs);
	CPU_SET(0, &cs);
	sched_setaffinity(0, sizeof(cs), &cs);

	of = open("/data/local/tmp/enf.out", O_WRONLY | O_TRUNC);
	m = snprintf(buf, sizeof(buf), "pre uid=%d of=%d\n", (int)getuid(), of);

	fd = open("/sys/fs/selinux/enforce", O_RDONLY);
	n = fd >= 0 ? (int)read(fd, b, sizeof(b) - 1) : -1;
	if (fd >= 0) close(fd);

	m += snprintf(buf + m, sizeof(buf) - m,
		      "post n=%d [%s] uid=%d euid=%d gid=%d\n",
		      n, n > 0 ? b : "-", (int)getuid(), (int)geteuid(),
		      (int)getgid());
	if (of >= 0) {
		write(of, buf, m);
		fsync(of);
		close(of);
	}
	return 0;
}
