/* Mali kbase reachability probe (safe, read-only ioctls):
 *  1) open /dev/mali0
 *  2) KBASE_IOCTL_VERSION_CHECK
 *  3) KBASE_IOCTL_MEM_FLAGS_CHANGE with bogus address (expect -EINVAL, proves ioctl reachable)
 * Mirrors mali_kbase_ioctl.h layouts for r18p0.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* r18p0 mali_kbase_ioctl.h:
 *   KBASE_IOCTL_TYPE 0x80
 *   KBASE_IOCTL_VERSION_CHECK _IOWR(type, 0, struct kbase_ioctl_version_check{u16,u16})
 *   KBASE_IOCTL_MEM_FLAGS_CHANGE _IOW(type, 23, struct kbase_ioctl_mem_flags_change{u64,u64,u64})
 */
struct kbase_ioctl_version_check {
	uint16_t major;
	uint16_t minor;
};

struct kbase_ioctl_mem_flags_change {
	uint64_t gpu_va;
	uint64_t flags;
	uint64_t mask;
};

#define KBASE_IOCTL_VERSION_CHECK _IOWR(0x80, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS _IOW(0x80, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_MEM_FLAGS_CHANGE _IOW(0x80, 23, struct kbase_ioctl_mem_flags_change)

struct kbase_ioctl_set_flags {
	uint32_t create_flags;
};

int main(void)
{
	int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("open /dev/mali0: %s\n", strerror(errno));
		return 1;
	}
	printf("open /dev/mali0 OK\n");

	struct kbase_ioctl_version_check vc = {0, 0};
	if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc) == 0)
		printf("VERSION_CHECK OK: driver %u.%u\n", vc.major, vc.minor);
	else
		printf("VERSION_CHECK errno=%d (%s)\n", errno, strerror(errno));

	/* create kernel context (required before MEM_FLAGS_CHANGE) */
	struct kbase_ioctl_set_flags sf;
	memset(&sf, 0, sizeof(sf));
	if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) == 0)
		printf("SET_FLAGS OK (kctx created)\n");
	else
		printf("SET_FLAGS errno=%d (%s)\n", errno, strerror(errno));

	/* benign flags change on a wild address: expect EINVAL, never a crash */
	struct kbase_ioctl_mem_flags_change fc;
	memset(&fc, 0, sizeof(fc));
	fc.gpu_va = 0xdead000000000000ull;
	fc.flags = 0;
	fc.mask = 0;
	int r = ioctl(fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc);
	printf("MEM_FLAGS_CHANGE(bogus) -> ret=%d errno=%d (%s)\n", r, errno, strerror(errno));

	close(fd);
	printf("done, still alive\n");
	return 0;
}
