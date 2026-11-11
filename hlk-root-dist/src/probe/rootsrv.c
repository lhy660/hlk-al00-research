/* rootsrv: minimal local root shell server for the temporary-root channel.
 *
 * Run as uid 0 from the kernel-domain rootd loop; accepts connections on
 * 127.0.0.1:31337 and hands each one to /system/bin/sh.  The intended use is
 *   adb forward tcp:31337 tcp:31337
 * followed by a client on the host.  It intentionally keeps running (this is
 * a daemon), so invoke it with setsid and redirect its stdio to /dev/null.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#define PORT 31337

/* Death recorder: every caught signal and every abnormal exit path appends
 * one line here, so a process that keeps dying in seconds can be pinned on
 * its killer (an uncatchable SIGKILL leaves NO line - its absence is the
 * answer).  Only active when HIDTRACE is set in the environment. */
static char trace_path[128];

static void trace(const char *what)
{
	int fd;
	if (!trace_path[0])
		return;
	fd = open(trace_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd >= 0) {
		char b[160];
		int n = snprintf(b, sizeof(b), "%s pid=%d uid=%d errno=%d\n",
				 what, (int)getpid(), (int)getuid(), errno);
		(void)!write(fd, b, (size_t)n);
		close(fd);
	}
}

static void die(int sig)
{
	trace("signal");
	_exit(128 + sig);
}

static void reap(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

int main(int argc, char **argv)
{
	int s, one = 1;
	int port = (argc > 1) ? atoi(argv[1]) : PORT;
	struct sockaddr_in a;
	const char *tp = getenv("HIDTRACE");

	if (tp && *tp) {
		strncpy(trace_path, tp, sizeof(trace_path) - 1);
		signal(SIGTERM, die);
		signal(SIGINT, die);
		signal(SIGHUP, die);
		signal(SIGSEGV, die);
		signal(SIGBUS, die);
		signal(SIGILL, die);
		signal(SIGABRT, die);
		signal(SIGPIPE, die);
	}

	signal(SIGCHLD, reap);

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(0x7f000001);	/* 127.0.0.1 */

	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		trace("bind-fail");
		perror("bind");
		return 2;
	}
	if (listen(s, 8) < 0) {
		trace("listen-fail");
		perror("listen");
		return 3;
	}
	fprintf(stderr, "rootsrv listening on 127.0.0.1:%d uid=%d\n",
		port, (int)getuid());
	trace("listening");

	for (;;) {
		int c = accept(s, NULL, NULL);
		pid_t p;

		if (c < 0) {
			if (errno == EINTR)
				continue;
			trace("accept-fail");
			perror("accept");
			usleep(100000);
			continue;
		}
		p = fork();
		if (p == 0) {
			dup2(c, 0);
			dup2(c, 1);
			dup2(c, 2);
			if (c > 2)
				close(c);
			close(s);
			execl("/system/bin/sh", "sh", "-i", (char *)NULL);
			_exit(127);
		}
		close(c);
	}
}
