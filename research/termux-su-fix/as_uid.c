/*
 * as_uid.c -- verification helper.
 *
 * Drops to a given app uid (default com.termux = 10161) and execs a command,
 * so the su/sudo/tsu chain is exercised with the same DAC identity as a real
 * Termux process.  Run it as root:
 *
 *   as_uid 10161 /data/data/com.termux/files/usr/bin/su -c id
 *
 * Note: setuid() does not change the SELinux domain or the session keyring;
 * for su-chain verification the uid is what matters (allowlist + file access).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <grp.h>
#include <sys/types.h>

int main(int argc, char **argv)
{
	uid_t target = 10161;
	char *cmd;

	if (argc >= 3 && strncmp(argv[1], "10", 2) == 0) {
		target = (uid_t)atoi(argv[1]);
		argv += 2;
		argc -= 2;
	} else {
		argv += 1;
		argc -= 1;
	}
	if (argc < 1) {
		fprintf(stderr, "usage: as_uid [UID] COMMAND [ARGS...]\n");
		return 2;
	}
	cmd = argv[0];

	/* Minimal Termux environment so tsu/bash can resolve their own files. */
	setenv("PREFIX", "/data/data/com.termux/files/usr", 1);
	setenv("HOME", "/data/data/com.termux/files/home", 1);
	setenv("TMPDIR", "/data/data/com.termux/files/usr/tmp", 1);
	setenv("LD_LIBRARY_PATH", "/data/data/com.termux/files/usr/lib", 1);
	setenv("PATH",
	       "/data/data/com.termux/files/usr/bin:"
	       "/data/data/com.termux/files/usr/bin/applets:"
	       "/system/bin:/system/xbin", 1);

	if (setgroups(0, NULL) != 0)
		fprintf(stderr, "[as_uid] setgroups: %s (continuing)\n",
			strerror(errno));
	if (setgid((gid_t)target) != 0) { perror("setgid"); return 1; }
	if (setuid(target) != 0)       { perror("setuid"); return 1; }

	fprintf(stderr, "[as_uid] now uid=%d gid=%d exec=%s\n",
		getuid(), getgid(), cmd);
	execvp(cmd, argv);
	fprintf(stderr, "[as_uid] execvp(%s): %s\n", cmd, strerror(errno));
	return 1;
}
