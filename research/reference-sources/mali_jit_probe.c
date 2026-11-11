/* Mali kbase JIT guard probe (safe, no shrinker pressure induced).
 *
 * Purpose: determine whether the running kernel enforces the
 * KBASE_REG_NO_USER_FREE guard in kbase_mem_flags_change() -- i.e. whether
 * the published CVE-2022-38181 primitive (marking a JIT region DONT_NEED
 * through MEM_FLAGS_CHANGE so it lands on the eviction list while jit_alloc[]
 * still references it) is closed on this device.
 *
 * Sequence:
 *   1. VERSION_CHECK 11.16
 *   2. SET_FLAGS (create kctx)
 *   3. mmap MTP (tracking page) so process_mm is set
 *   4. MEM_JIT_INIT (64 VA pages, 8 allocs, trim 0, group 0)
 *   5. MEM_ALLOC 1 page (SAME_VA) + mmap -> buffer whose GPU VA receives
 *      the JIT allocation address
 *   6. JOB_SUBMIT softjob JIT_ALLOC(id=1, 4 pages, extent 1)
 *   7. MEM_FLAGS_CHANGE(JIT_addr, DONT_NEED, DONT_NEED)
 *        -> EINVAL == guard present (fixed)
 *        -> 0      == guard absent  (CVE-2022-38181 primitive reachable)
 *   8. cleanup: undo flags if needed, JIT_FREE id 1
 *   9. positive control on a plain native region: same flags change must
 *      succeed, proving the ioctl path itself works.
 *
 * Deliberately does NOT force memory pressure, so the shrinker is not
 * induced and no free-while-evictable race is triggered.
 *
 * ABI mirrors bifrost-r18p0-01rel0 mali_kbase_ioctl.h / mali_base_kernel.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define KBASE_IOCTL_TYPE 0x80

struct kbase_ioctl_version_check {
	uint16_t major;
	uint16_t minor;
};

struct kbase_ioctl_set_flags {
	uint32_t create_flags;
};

struct kbase_ioctl_job_submit {
	uint64_t addr;
	uint32_t nr_atoms;
	uint32_t stride;
};

union kbase_ioctl_mem_alloc {
	struct {
		uint64_t va_pages;
		uint64_t commit_pages;
		uint64_t extent;
		uint64_t flags;
	} in;
	struct {
		uint64_t flags;
		uint64_t gpu_va;
	} out;
};

struct kbase_ioctl_mem_jit_init {
	uint64_t va_pages;
	uint8_t max_allocations;
	uint8_t trim_level;
	uint8_t group_id;
	uint8_t padding[5];
};

struct kbase_ioctl_mem_flags_change {
	uint64_t gpu_va;
	uint64_t flags;
	uint64_t mask;
};

/* ---- base ABI (layout-critical) ---- */
struct base_jd_udata {
	uint64_t blob[2];
};

struct base_dependency {
	uint8_t atom_id;
	uint8_t dependency_type;
};

struct base_jd_atom_v2 {
	uint64_t jc;
	struct base_jd_udata udata;
	uint64_t extres_list;
	uint16_t nr_extres;
	uint16_t compat_core_req;
	struct base_dependency pre_dep[2];
	uint8_t atom_number;
	uint8_t prio;
	uint8_t device_nr;
	uint8_t padding[1];
	uint32_t core_req;
};

_Static_assert(sizeof(struct base_jd_atom_v2) == 48, "atom size must be 48");

struct base_jd_event_v2 {
	uint32_t event_code;
	uint8_t atom_number;
	uint8_t padding[3];
	struct base_jd_udata udata;
};

struct base_jit_alloc_info {
	uint64_t gpu_alloc_addr;
	uint64_t va_pages;
	uint64_t commit_pages;
	uint64_t extent;
	uint8_t id;
	uint8_t bin_id;
	uint8_t max_allocations;
	uint8_t flags;
	uint8_t padding[2];
	uint16_t usage_id;
};

_Static_assert(sizeof(struct base_jit_alloc_info) == 40, "jit info must be 40");

#define KBASE_IOCTL_VERSION_CHECK \
	_IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS \
	_IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_JOB_SUBMIT \
	_IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)
#define KBASE_IOCTL_MEM_ALLOC \
	_IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define KBASE_IOCTL_MEM_JIT_INIT \
	_IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)
#define KBASE_IOCTL_MEM_FLAGS_CHANGE \
	_IOW(KBASE_IOCTL_TYPE, 23, struct kbase_ioctl_mem_flags_change)

#define BASE_MEM_PROT_CPU_RD (1ull << 0)
#define BASE_MEM_PROT_CPU_WR (1ull << 1)
#define BASE_MEM_PROT_GPU_RD (1ull << 2)
#define BASE_MEM_PROT_GPU_WR (1ull << 3)
#define BASE_MEM_SAME_VA     (1ull << 13)
#define BASE_MEM_DONT_NEED   (1ull << 17)

#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)

#define BASE_JD_REQ_SOFT_JIT_ALLOC (1u << 9 | 0x9)
#define BASE_JD_REQ_SOFT_JIT_FREE  (1u << 9 | 0xa)
#define BASE_JD_EVENT_DONE 0x01

static int map_tracking_page(int fd)
{
	void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       (off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	if (p == MAP_FAILED) {
		printf("TRACKING mmap failed: errno=%d (%s)\n", errno,
		       strerror(errno));
		return -1;
	}
	printf("TRACKING page mapped at %p\n", p);
	return 0;
}

/* returns GPU VA (== user CPU address for SAME_VA) or 0 */
static uint64_t alloc_page(int fd, const char *tag)
{
	union kbase_ioctl_mem_alloc a;
	void *p;
	uint64_t gpu_va;

	memset(&a, 0, sizeof(a));
	a.in.va_pages = 1;
	a.in.commit_pages = 1;
	a.in.extent = 0;
	a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
		     BASE_MEM_SAME_VA;

	if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a) != 0) {
		printf("%s: MEM_ALLOC errno=%d (%s)\n", tag, errno,
		       strerror(errno));
		return 0;
	}
	printf("%s: MEM_ALLOC ok cookie=0x%llx out_flags=0x%llx\n", tag,
	       (unsigned long long)a.out.gpu_va,
	       (unsigned long long)a.out.flags);

	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)a.out.gpu_va);
	if (p == MAP_FAILED) {
		printf("%s: buffer mmap failed: errno=%d (%s)\n", tag, errno,
		       strerror(errno));
		return 0;
	}
	gpu_va = (uint64_t)(uintptr_t)p;
	*(volatile uint64_t *)p = 0;
	printf("%s: buffer GPU VA = 0x%llx\n", tag,
	       (unsigned long long)gpu_va);
	return gpu_va;
}

static int read_event(int fd, struct base_jd_event_v2 *ev)
{
	int n = (int)read(fd, ev, sizeof(*ev));
	if (n != (int)sizeof(*ev))
		return -1;
	return 0;
}

static int submit_one(int fd, struct base_jd_atom_v2 *atom)
{
	struct kbase_ioctl_job_submit js;

	memset(&js, 0, sizeof(js));
	js.addr = (uint64_t)(uintptr_t)atom;
	js.nr_atoms = 1;
	js.stride = sizeof(*atom);
	return ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &js);
}

static int flags_change(int fd, uint64_t gpu_va, uint64_t flags)
{
	struct kbase_ioctl_mem_flags_change fc;

	memset(&fc, 0, sizeof(fc));
	fc.gpu_va = gpu_va;
	fc.flags = flags;
	fc.mask = BASE_MEM_DONT_NEED;
	return ioctl(fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc);
}

int main(void)
{
	int fd;
	struct kbase_ioctl_version_check vc;
	struct kbase_ioctl_set_flags sf;
	struct kbase_ioctl_mem_jit_init ji;
	uint64_t buf_va = 0, jit_va = 0;
	struct base_jit_alloc_info info;
	struct base_jd_atom_v2 atom;
	struct base_jd_event_v2 ev;
	uint8_t jit_id = 1;
	int ret;

	setbuf(stdout, NULL);

	fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("open /dev/mali0: %s\n", strerror(errno));
		return 1;
	}
	printf("open /dev/mali0 OK (fd=%d)\n", fd);

	memset(&vc, 0, sizeof(vc));
	vc.major = 11;
	vc.minor = 16;
	if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc) != 0)
		printf("VERSION_CHECK errno=%d (%s)\n", errno, strerror(errno));
	else
		printf("VERSION_CHECK -> driver %u.%u\n", vc.major, vc.minor);

	memset(&sf, 0, sizeof(sf));
	if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) != 0) {
		printf("SET_FLAGS errno=%d (%s)\n", errno, strerror(errno));
		return 1;
	}
	printf("SET_FLAGS OK (kctx created)\n");

	if (map_tracking_page(fd) != 0)
		return 1;

	/* JIT init must precede any region allocation. */
	memset(&ji, 0, sizeof(ji));
	ji.va_pages = 64;
	ji.max_allocations = 8;
	ji.trim_level = 0;
	ji.group_id = 0;
	if (ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &ji) != 0) {
		printf("JIT_INIT errno=%d (%s)  [continuing anyway]\n", errno,
		       strerror(errno));
	} else {
		printf("JIT_INIT OK (64 VA pages, max 8 allocs)\n");
	}

	buf_va = alloc_page(fd, "JITADDR-BUF");
	if (!buf_va)
		return 1;

	/* Submit JIT_ALLOC softjob: id 1, 4 pages, extent 1. */
	memset(&info, 0, sizeof(info));
	info.gpu_alloc_addr = buf_va;
	info.va_pages = 4;
	info.commit_pages = 4;
	info.extent = 1;
	info.id = 1;
	info.bin_id = 0;
	info.max_allocations = 8;
	info.flags = 0;
	info.usage_id = 0;

	memset(&atom, 0, sizeof(atom));
	atom.jc = (uint64_t)(uintptr_t)&info;
	atom.nr_extres = 1;
	atom.atom_number = 1;
	atom.prio = 0;
	atom.core_req = BASE_JD_REQ_SOFT_JIT_ALLOC;

	ret = submit_one(fd, &atom);
	printf("JOB_SUBMIT(JIT_ALLOC) -> ret=%d errno=%d (%s)\n", ret, errno,
	       strerror(errno));
	if (ret != 0)
		return 1;

	if (read_event(fd, &ev) == 0)
		printf("event: code=0x%x atom=%u\n", ev.event_code,
		       ev.atom_number);
	else
		printf("event: none (nonblocking)\n");

	jit_va = *(volatile uint64_t *)(uintptr_t)buf_va;
	printf("JIT allocation GPU VA = 0x%llx\n", (unsigned long long)jit_va);

	if (!jit_va) {
		printf("RESULT: JIT_ALLOC did not produce an address; "
		       "cannot test guard\n");
		return 2;
	}

	/* ---- the actual guard test ---- */
	errno = 0;
	ret = flags_change(fd, jit_va, BASE_MEM_DONT_NEED);
	printf("MEM_FLAGS_CHANGE(JIT, DONT_NEED) -> ret=%d errno=%d (%s)\n",
	       ret, errno, strerror(errno));

	if (ret != 0 && errno == EINVAL) {
		printf("RESULT: NO_USER_FREE guard PRESENT (fixed): "
		       "CVE-2022-38181 flags-change primitive closed\n");
	} else if (ret == 0) {
		printf("RESULT: guard ABSENT -- JIT region marked evictable! "
		       "undoing immediately\n");
		ret = flags_change(fd, jit_va, 0);
		printf("  undo flags_change -> ret=%d errno=%d (%s)\n", ret,
		       errno, strerror(errno));
	} else {
		printf("RESULT: unexpected error, treating as inconclusive\n");
	}

	/* Release JIT allocation through the intended path. */
	memset(&atom, 0, sizeof(atom));
	atom.jc = (uint64_t)(uintptr_t)&jit_id;
	atom.nr_extres = 1;
	atom.atom_number = 2;
	atom.prio = 0;
	atom.core_req = BASE_JD_REQ_SOFT_JIT_FREE;
	ret = submit_one(fd, &atom);
	printf("JOB_SUBMIT(JIT_FREE id=1) -> ret=%d errno=%d (%s)\n", ret,
	       errno, strerror(errno));
	if (ret == 0 && read_event(fd, &ev) == 0)
		printf("event: code=0x%x atom=%u\n", ev.event_code,
		       ev.atom_number);

	/* ---- positive control: plain native region must accept DONT_NEED ---- */
	{
		uint64_t ctl = alloc_page(fd, "CONTROL");
		if (ctl) {
			errno = 0;
			ret = flags_change(fd, ctl, BASE_MEM_DONT_NEED);
			printf("CONTROL MEM_FLAGS_CHANGE(DONT_NEED) -> "
			       "ret=%d errno=%d (%s)\n", ret, errno,
			       strerror(errno));
			if (ret == 0) {
				printf("  (positive control OK: ioctl path "
				       "works, guard is JIT-specific)\n");
				ret = flags_change(fd, ctl, 0);
				printf("  undo -> ret=%d errno=%d (%s)\n",
				       ret, errno, strerror(errno));
			}
		}
	}

	printf("probe done, closing\n");
	close(fd);
	printf("closed, still alive\n");
	return 0;
}