/*
 * ksu_probe.c -- standalone end-to-end test of hlk_ksu_shim's supercall ABI.
 *
 * Prints its PID, waits for /data/local/tmp/ksu_go, then scans /proc/self/fd
 * for the injected [ksu_driver] anon-inode fd and issues GET_INFO and
 * GRANT_ROOT. This verifies the kernel side without depending on the
 * SukiSU Manager UI (which crashes on Android 10 due to a splash drawable).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>

struct ksu_get_info_cmd {
	unsigned int version;
	unsigned int flags;
	unsigned int features;
	unsigned int uapi_version;
};

#define KSU_IOCTL_GRANT_ROOT	_IO('K', 1)
#define KSU_IOCTL_GET_INFO	_IOR('K', 2, struct ksu_get_info_cmd)

static int scan_driver_fd(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *de;
	int found = -1;

	if (!d)
		return -1;
	while ((de = readdir(d)) != NULL) {
		char path[64], target[256];
		ssize_t n;
		int fd;

		if (de->d_name[0] == '.')
			continue;
		fd = atoi(de->d_name);
		snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
		n = readlink(path, target, sizeof(target) - 1);
		if (n < 0)
			continue;
		target[n] = '\0';
		if (strstr(target, "[ksu_driver]")) {
			found = fd;
			break;
		}
	}
	closedir(d);
	return found;
}

int main(void)
{
	struct ksu_get_info_cmd info;
	int fd, rc, tries = 0;

	printf("PID=%d UID_BEFORE=%d\n", getpid(), getuid());
	fflush(stdout);

	while (access("/data/local/tmp/ksu_go", F_OK) != 0) {
		usleep(100000);
		if (++tries > 300) {
			printf("TIMEOUT waiting for ksu_go\n");
			return 2;
		}
	}

	fd = scan_driver_fd();
	printf("DRIVER_FD=%d\n", fd);
	fflush(stdout);
	if (fd < 0)
		return 3;

	memset(&info, 0, sizeof(info));
	errno = 0;
	rc = ioctl(fd, KSU_IOCTL_GET_INFO, &info);
	printf("GET_INFO rc=%d errno=%d version=%u flags=0x%x features=%u uapi=%u\n",
	       rc, errno, info.version, info.flags, info.features,
	       info.uapi_version);
	fflush(stdout);

	errno = 0;
	rc = ioctl(fd, KSU_IOCTL_GRANT_ROOT, 0);
	printf("GRANT_ROOT rc=%d errno=%d UID_AFTER=%d\n", rc, errno, getuid());
	fflush(stdout);
	return 0;
}
