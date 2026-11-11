/*
 * hlk_su.c -- request-based root entry point for allowlisted apps.
 *
 * This is the userspace half of the SukiSU v4.2.1 shim (hlk_ksu_shim8).
 * The kernel shim does NOT auto-escalate app processes any more: doing that
 * on the first syscall of a freshly forked app turned it into uid 0 before
 * it attached to system_server, and the framework killed it.
 *
 * Instead a granted app requests root explicitly:
 *   1. find the [ksu_driver] anon-inode fd (auto-installed by the shim into
 *      allowlisted uids), or ask the kernel for one via reboot(MAGIC1,MAGIC2).
 *   2. ioctl(fd, KSU_IOCTL_GRANT_ROOT) -- the shim checks the allowlist and
 *      only then commits init_cred and switches to u:r:shell:s0.
 *   3. exec /system/bin/sh (optionally with -c <command>).
 *
 * Build: aarch64-linux-android29-clang -static -O2 -o hlk_su hlk_su.c
 * Deploy: /data/local/tmp/hlk_su  (mode 0755)
 *
 * --as-uid <uid> is a verification helper: drop from root to that uid
 * before requesting, so the allowlist gating can be tested end-to-end.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>

#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT		47
#endif
#ifndef PR_CAP_AMBIENT_RAISE
#define PR_CAP_AMBIENT_RAISE	2
#endif
/* Linux 4.14 (HLK-AL00): CAP_LAST_CAP = 36. */
#define HLK_CAP_LAST		36

#define KSU_MAGIC1		0xDEADBEEFu
#define KSU_MAGIC2		0xCAFEBABEu
/* _IOC(_IOC_NONE, 'K', 1, 0) */
#define KSU_IOCTL_GRANT_ROOT	0x4B01u

/*
 * The shim now grants root with prepare_creds() so the FBE session keyring
 * survives (no more ENOKEY).  That also means the caller's capability
 * bounding set is inherited; on this device adb shell starts with
 * CapBnd=0xc0, and cap_bprm_set_creds() recomputes pP'/pE' during execve()
 * in a way that leaves the resulting shell with CapEff=0 -- privileged
 * operations then fail with EPERM even though uid==0.
 *
 * Raising every cap into the ambient set before execve() makes
 * cap_bprm_set_creds() derive full permitted/effective caps from pA'.
 * Verified on device: CapEff=1fffffffff survives execve().
 * Harmless no-op on kernels without PR_CAP_AMBIENT.
 */
static void keep_caps_across_exec(void)
{
	int cap;

	for (cap = 0; cap <= HLK_CAP_LAST; cap++)
		(void)prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE,
			    (unsigned long)cap, 0UL, 0UL, 0UL);
}

static int scan_driver_fd(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *de;
	char path[64], target[PATH_MAX];
	int found = -1;

	if (!d)
		return -1;
	while ((de = readdir(d)) != NULL) {
		char *end;
		long v;
		ssize_t n;

		if (de->d_name[0] == '.')
			continue;
		v = strtol(de->d_name, &end, 10);
		if (*end != '\0' || v < 0 || v > INT_MAX)
			continue;
		snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
		n = readlink(path, target, sizeof(target) - 1);
		if (n < 0)
			continue;
		target[n] = '\0';
		if (strstr(target, "[ksu_driver]")) {
			found = (int)v;
			break;
		}
	}
	closedir(d);
	return found;
}

int main(int argc, char **argv)
{
	int as_uid = -1, fd, i, len = 0;
	char *cmd = NULL;
	char *sh_argv[5];

	if (argc >= 3 && strcmp(argv[1], "--as-uid") == 0) {
		as_uid = atoi(argv[2]);
		argv += 2;
		argc -= 2;
	}
	if (as_uid > 0) {
		setgid((gid_t)as_uid);
		if (setuid((uid_t)as_uid) != 0) {
			perror("hlk_su: setuid");
			return 1;
		}
	}

	fd = scan_driver_fd();
	if (fd < 0) {
		int nfd = -1;

		/* The shim's sys_enter probe installs the fd via task_work. */
		syscall(SYS_reboot, KSU_MAGIC1, KSU_MAGIC2, 0, &nfd);
		fd = nfd;
	}
	if (fd < 0) {
		fprintf(stderr, "hlk_su: no [ksu_driver] fd (uid=%d)\n", getuid());
		return 1;
	}

	if (getuid() != 0) {
		if (ioctl(fd, KSU_IOCTL_GRANT_ROOT) != 0) {
			fprintf(stderr,
				"hlk_su: GRANT_ROOT denied: %s (uid=%d, fd=%d)\n",
				strerror(errno), getuid(), fd);
			return 1;
		}
		if (getuid() != 0) {
			fprintf(stderr, "hlk_su: still uid=%d after grant\n",
				getuid());
			return 1;
		}
	}

	/* Must run after GRANT_ROOT (caps are full now) and before execv(). */
	keep_caps_across_exec();

	if (argc > 1) {
		if (argc == 2 || strcmp(argv[1], "-c") != 0) {
			for (i = 1; i < argc; i++)
				len += (int)strlen(argv[i]) + 1;
			cmd = calloc(1, len + 1);
			if (!cmd)
				return 1;
			for (i = 1; i < argc; i++) {
				strcat(cmd, argv[i]);
				if (i + 1 < argc)
					strcat(cmd, " ");
			}
		} else {
			cmd = argv[2];
		}
		if (cmd) {
			sh_argv[0] = "/system/bin/sh";
			sh_argv[1] = "-c";
			sh_argv[2] = cmd;
			sh_argv[3] = NULL;
		} else {
			sh_argv[0] = "/system/bin/sh";
			sh_argv[1] = NULL;
		}
	} else {
		sh_argv[0] = "/system/bin/sh";
		sh_argv[1] = NULL;
	}
	execv(sh_argv[0], sh_argv);
	perror("hlk_su: execv");
	return 1;
}
