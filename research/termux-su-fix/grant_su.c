/*
 * grant_su.c -- add/remove a uid in hlk_ksu_shim9's in-kernel su allowlist.
 *
 * The shim's allowlist lives in module memory only: it is empty after every
 * reboot and there is no on-disk persistence (Manager's settings.xml only
 * holds ui_mode).  Run this as root (or uid 2000 / manager uid) to restore
 * the grant, e.g. from deploy.sh after a reboot:
 *
 *   grant_su 10161 allow com.termux
 *   grant_su 10161 deny
 *
 * ioctl: KSU_IOCTL_SET_APP_PROFILE = _IOC(_IOC_WRITE, 'K', 12, 0)
 * struct app_profile { __u32 version; char key[256]; __s32 curr_uid;
 *                      bool allow_su; ... };  sizeof == 784
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
#include <sys/syscall.h>
#include <sys/types.h>

#define KSU_MAGIC1			0xDEADBEEFu
#define KSU_MAGIC2			0xCAFEBABEu
#define KSU_IOCTL_SET_APP_PROFILE	0x40004B0Cu

#define OFF_KEY				4
#define OFF_UID				260
#define OFF_ALLOW			264
#define PROFILE_SIZE			784

static int find_driver_fd(void)
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
		if (*end || v < 0 || v > INT_MAX)
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
	unsigned char *buf;
	int fd, nfd = -1, allow = 1;
	unsigned int uid;

	if (argc < 3) {
		fprintf(stderr, "usage: grant_su UID allow|deny [KEY]\n");
		return 2;
	}
	uid = (unsigned int)strtoul(argv[1], NULL, 10);
	if (strcmp(argv[2], "deny") == 0)
		allow = 0;

	buf = calloc(1, PROFILE_SIZE);
	if (!buf)
		return 1;
	memcpy(buf + OFF_KEY, argc > 3 ? argv[3] : "",
	       strnlen(argc > 3 ? argv[3] : "", 255));
	*(unsigned int *)(buf + OFF_UID) = uid;
	buf[OFF_ALLOW] = (unsigned char)allow;

	fd = find_driver_fd();
	if (fd < 0) {
		syscall(SYS_reboot, KSU_MAGIC1, KSU_MAGIC2, 0, &nfd);
		fd = nfd;
	}
	if (fd < 0) {
		fprintf(stderr, "grant_su: no [ksu_driver] fd (uid=%d)\n",
			getuid());
		return 1;
	}
	if (ioctl(fd, KSU_IOCTL_SET_APP_PROFILE, buf) != 0) {
		fprintf(stderr, "grant_su: SET_APP_PROFILE failed: %s (uid=%d)\n",
			strerror(errno), getuid());
		return 1;
	}
	printf("grant_su: uid=%u allow_su=%d key=%s OK\n",
	       uid, allow, argc > 3 ? argv[3] : "(none)");
	return 0;
}
