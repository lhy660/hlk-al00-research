/* hlk_execsu -- call execve("/system/bin/su") directly, bypassing shell PATH
 * search, so the sucompat redirect can be tested end to end.
 * Standard apps (Runtime.exec) end up doing exactly this after PATH lookup.
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

extern char **environ;

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "/system/bin/su";
	char *av[] = { (char *)"su", (char *)"-c", (char *)"id", NULL };

	/* sucompat 还必须在 access()/stat() 上撒谎，否则探测型调用方会提前放弃 */
	printf("access=%d ", access(path, X_OK));
	{
		struct stat st;

		printf("stat=%d ", stat(path, &st));
	}
	printf("execve->\n");
	fflush(stdout);

	execve(path, av, environ);
	fprintf(stderr, "execve(%s) failed: %s (errno=%d)\n",
		path, strerror(errno), errno);
	return 127;
}
