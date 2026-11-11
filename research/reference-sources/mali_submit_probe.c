/* Diagnose JOB_SUBMIT EINVAL on HLK-AL00 (HM2, kbase 11.16).
 *
 * Hypotheses for -EINVAL from kbase_jd_submit():
 *   a) stride != kernel sizeof(base_jd_atom_v2)
 *   b) KCTX_SUBMIT_DISABLED (context killed / unsupported)
 *   c) copy_from_user failure (should not happen for our stack atom)
 *
 * Tests, in order:
 *   0) nr_atoms=0, stride=56        -> separates (b) from (a)/(c)
 *   1) nr_atoms=1, stride variants  -> finds kernel atom size if (a)
 *
 * The atom is a valid JIT_ALLOC softjob; each attempt uses a fresh JIT id.
 * No GPU memory pressure is created.
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

struct kbase_ioctl_version_check { uint16_t major, minor; };
struct kbase_ioctl_set_flags { uint32_t create_flags; };
struct kbase_ioctl_job_submit { uint64_t addr; uint32_t nr_atoms; uint32_t stride; };
union kbase_ioctl_mem_alloc {
	struct { uint64_t va_pages, commit_pages, extent, flags; } in;
	struct { uint64_t flags, gpu_va; } out;
};
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
	uint8_t thro;
};
_Static_assert(sizeof(struct base_jd_atom_v2) == 56, "atom must be 56");

struct base_jit_alloc_info {
	uint64_t gpu_alloc_addr, va_pages, commit_pages, extent;
	uint8_t id, bin_id, max_allocations, flags, padding[2];
	uint16_t usage_id;
};
_Static_assert(sizeof(struct base_jit_alloc_info) == 40, "jit info must be 40");

struct base_jd_event_v2 {
	uint32_t event_code;
	uint8_t atom_number, padding[3];
	struct base_jd_udata udata;
};

#define KBASE_IOCTL_VERSION_CHECK _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_JOB_SUBMIT _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)
#define KBASE_IOCTL_MEM_ALLOC _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define KBASE_IOCTL_MEM_JIT_INIT _IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)

#define BASE_MEM_PROT_CPU_RD (1ull << 0)
#define BASE_MEM_PROT_CPU_WR (1ull << 1)
#define BASE_MEM_PROT_GPU_RD (1ull << 2)
#define BASE_MEM_PROT_GPU_WR (1ull << 3)
#define BASE_MEM_SAME_VA     (1ull << 13)
#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)
#define BASE_JD_REQ_SOFT_JIT_ALLOC (1u << 9 | 0x9)

static int do_submit(int fd, struct base_jd_atom_v2 *atom, uint32_t nr, uint32_t stride)
{
	struct kbase_ioctl_job_submit js;
	memset(&js, 0, sizeof(js));
	js.addr = (uint64_t)(uintptr_t)atom;
	js.nr_atoms = nr;
	js.stride = stride;
	errno = 0;
	return ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &js);
}

int main(void)
{
	int fd, ret;
	struct kbase_ioctl_version_check vc;
	struct kbase_ioctl_set_flags sf;
	struct kbase_ioctl_mem_jit_init ji;
	union kbase_ioctl_mem_alloc a;
	struct base_jit_alloc_info info;
	struct base_jd_atom_v2 atom;
	void *tracking, *buf;
	uint64_t buf_va;
	static const uint32_t strides[] = {56, 64, 48, 72, 0};
	char ids[sizeof(strides)/sizeof(strides[0])];

	setbuf(stdout, NULL);
	printf("user sizeof(atom)=%zu sizeof(jit_info)=%zu\n",
	       sizeof(struct base_jd_atom_v2), sizeof(struct base_jit_alloc_info));

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

	tracking = mmap(NULL, 0x1000, 0, MAP_SHARED, fd, (off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	printf("tracking mmap=%p errno=%d\n", tracking, errno);
	if (tracking == MAP_FAILED) return 1;

	memset(&ji, 0, sizeof(ji));
	ji.va_pages = 64; ji.max_allocations = 16; ji.trim_level = 0; ji.group_id = 0;
	ret = ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &ji);
	printf("JIT_INIT ret=%d errno=%d (%s)\n", ret, errno, strerror(errno));
	if (ret) return 1;

	memset(&a, 0, sizeof(a));
	a.in.va_pages = 1; a.in.commit_pages = 1;
	a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
	ret = ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a);
	printf("MEM_ALLOC ret=%d cookie=0x%llx errno=%d\n", ret,
	       (unsigned long long)a.out.gpu_va, errno);
	if (ret) return 1;
	buf = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)a.out.gpu_va);
	if (buf == MAP_FAILED) { printf("buf mmap: %s\n", strerror(errno)); return 1; }
	buf_va = (uint64_t)(uintptr_t)buf;
	*(volatile uint64_t *)buf = 0;
	printf("buf GPU VA=0x%llx\n", (unsigned long long)buf_va);

	/* 0) nr_atoms=0, stride 56: only SUBMIT_DISABLED/stride checks run */
	memset(&atom, 0, sizeof(atom));
	atom.atom_number = 100;
	atom.core_req = BASE_JD_REQ_SOFT_JIT_ALLOC;
	atom.nr_extres = 1;
	ret = do_submit(fd, &atom, 0, 56);
	printf("SUBMIT(nr=0,stride=56) ret=%d errno=%d (%s)\n", ret, errno, strerror(errno));

	/* 1) real JIT_ALLOC per stride candidate */
	for (unsigned i = 0; i < sizeof(strides)/sizeof(strides[0]); i++) {
		uint8_t id = (uint8_t)(10 + i);
		memset(&info, 0, sizeof(info));
		info.gpu_alloc_addr = buf_va;
		info.va_pages = 4;
		info.commit_pages = 4;
		info.extent = 1;
		info.id = id;
		info.max_allocations = 16;
		memset(&atom, 0, sizeof(atom));
		atom.jc = (uint64_t)(uintptr_t)&info;
		atom.nr_extres = 1;
		atom.atom_number = (uint8_t)(20 + i);
		atom.core_req = BASE_JD_REQ_SOFT_JIT_ALLOC;
		*(volatile uint64_t *)buf = 0;
		ret = do_submit(fd, &atom, 1, strides[i]);
		printf("SUBMIT(stride=%u,id=%u) ret=%d errno=%d (%s) buf=0x%llx\n",
		       strides[i], id, ret, errno, strerror(errno),
		       (unsigned long long)*(volatile uint64_t *)buf);
		ids[i] = (char)id;
		usleep(20000);
	}

	/* 2) dump a pending event if any */
	{
		struct base_jd_event_v2 ev;
		int n = (int)read(fd, &ev, sizeof(ev));
		if (n == (int)sizeof(ev))
			printf("event code=0x%x atom=%u\n", ev.event_code, ev.atom_number);
		else
			printf("no event (n=%d errno=%d)\n", n, errno);
	}

	printf("done, still alive\n");
	close(fd);
	return 0;
}