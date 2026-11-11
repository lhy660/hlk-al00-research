/* HLK-AL00 isolated MEM_ALIAS probe.
 *
 * Purpose: retest the alias path after the previous stage-2 run lost ADB at
 * section E, in a fresh process with step-by-step output and SIGBUS/SIGSEGV
 * capture so a bad userspace access cannot silently kill the run.
 *
 * Known kernel behaviour (HM2 11.16, mali_kbase_mem_linux.c):
 *   - a single VMA over a multi-entry alias can only CPU-fault pages of the
 *     *first* aliased entry; accessing a later stride slot of that VMA yields
 *     SIGBUS by design (get_aliased_alloc() + fault_pgoff >= nents).
 *   - the public exploit therefore re-mmaps each stride chunk separately; a
 *     chunk VMA starts at that chunk's offset and faults correctly.
 * This probe reproduces exactly that pattern, small: 2 sources x 4 pages,
 * stride 4 pages.
 *
 * Build:
 *   tools/zig-windows-x86_64-0.11.0/zig.exe cc -target aarch64-linux-musl \
 *     -static -O2 -o probe/mali_alias_probe probe/mali_alias_probe.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define KBASE_IOCTL_TYPE 0x80

struct kbase_ioctl_version_check { uint16_t major, minor; };
struct kbase_ioctl_set_flags { uint32_t create_flags; };
union kbase_ioctl_mem_alloc {
	struct { uint64_t va_pages, commit_pages, extent, flags; } in;
	struct { uint64_t flags, gpu_va; } out;
};
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

#define KBASE_IOCTL_VERSION_CHECK _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_MEM_ALLOC _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define KBASE_IOCTL_MEM_ALIAS _IOWR(KBASE_IOCTL_TYPE, 21, union kbase_ioctl_mem_alias)

#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)
#define BASE_MEM_PROT_CPU_RD (1ull << 0)
#define BASE_MEM_PROT_CPU_WR (1ull << 1)
#define BASE_MEM_PROT_GPU_RD (1ull << 2)
#define BASE_MEM_PROT_GPU_WR (1ull << 3)

#define PAGE 4096ull
#define STRIDE 4ull          /* pages per alias entry, like SPRAY_PAGES */

static sigjmp_buf jb;
static volatile sig_atomic_t in_guard;
static volatile uintptr_t fault_addr;

static void fault_handler(int sig, siginfo_t *si, void *uc)
{
	(void)sig; (void)uc;
	if (in_guard) {
		fault_addr = (uintptr_t)si->si_addr;
		in_guard = 0;
		siglongjmp(jb, 1);
	}
	/* Unexpected fault outside a guarded read: die loudly. */
	_exit(42);
}

/* Returns 0 and stores value on success, -1 on SIGBUS/SIGSEGV. */
static int guarded_read64(const volatile uint64_t *p, uint64_t *out)
{
	fault_addr = 0;
	in_guard = 1;
	if (sigsetjmp(jb, 1) == 0) {
		*out = *p;
		in_guard = 0;
		return 0;
	}
	in_guard = 0;
	return -1;
}

static int open_mali(void)
{
	int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
	struct kbase_ioctl_version_check vc;
	struct kbase_ioctl_set_flags sf;
	void *tracking;

	if (fd < 0) {
		printf("open /dev/mali0: %s\n", strerror(errno));
		return -1;
	}
	memset(&vc, 0, sizeof(vc));
	vc.major = 11; vc.minor = 16;
	if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &vc) != 0)
		printf("  VERSION_CHECK failed: %s\n", strerror(errno));
	printf("  version %u.%u\n", vc.major, vc.minor);

	memset(&sf, 0, sizeof(sf));
	if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) != 0) {
		printf("  SET_FLAGS failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	tracking = mmap(NULL, PAGE, 0, MAP_SHARED, fd,
			(off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	if (tracking == MAP_FAILED) {
		printf("  tracking mmap failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	printf("  tracking page at %p\n", tracking);
	return fd;
}

/* Allocate a native region, mmap it RW, fill every page with `pat`. */
static int alloc_src(int fd, uint64_t pages, uint64_t pat, uint64_t *va)
{
	union kbase_ioctl_mem_alloc a;
	void *p;

	memset(&a, 0, sizeof(a));
	a.in.va_pages = pages;
	a.in.commit_pages = pages;
	a.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	errno = 0;
	if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &a) != 0) {
		printf("  MEM_ALLOC(%llu pages) failed: errno=%d (%s)\n",
		       (unsigned long long)pages, errno, strerror(errno));
		return -1;
	}
	p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)a.out.gpu_va);
	if (p == MAP_FAILED) {
		printf("  mmap src failed: errno=%d (%s)\n", errno, strerror(errno));
		return -1;
	}
	for (uint64_t i = 0; i < pages; i++)
		((volatile uint64_t *)((char *)p + i * PAGE))[0] = pat;
	*va = (uint64_t)(uintptr_t)p;
	printf("  src: cookie=0x%llx va=0x%llx pages=%llu pat=0x%llx\n",
	       (unsigned long long)a.out.gpu_va, (unsigned long long)*va,
	       (unsigned long long)pages, (unsigned long long)pat);
	return 0;
}

static int make_alias(int fd, uint64_t *src, uint64_t nents, uint64_t len,
		      uint64_t *alias_cookie, uint64_t *alias_pages)
{
	struct base_mem_aliasing_info ai[8];
	union kbase_ioctl_mem_alias al;

	if (nents > 8)
		return -1;
	memset(ai, 0, sizeof(ai));
	for (uint64_t i = 0; i < nents; i++) {
		ai[i].handle.basep.handle = src[i];
		ai[i].offset = 0;
		ai[i].length = len;
	}
	memset(&al, 0, sizeof(al));
	al.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
		      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
	al.in.stride = STRIDE;
	al.in.nents = nents;
	al.in.aliasing_info = (uint64_t)(uintptr_t)ai;
	errno = 0;
	if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &al) != 0) {
		printf("  MEM_ALIAS(nents=%llu) failed: errno=%d (%s)\n",
		       (unsigned long long)nents, errno, strerror(errno));
		return -1;
	}
	printf("  alias out: flags=0x%llx cookie=0x%llx pages=%llu\n",
	       (unsigned long long)al.out.flags,
	       (unsigned long long)al.out.gpu_va,
	       (unsigned long long)al.out.va_pages);
	*alias_cookie = al.out.gpu_va;
	*alias_pages = al.out.va_pages;
	return 0;
}

/* nents=1: whole alias is one chunk, no SIGBUS expected anywhere. */
static void test_single(int fd)
{
	uint64_t src, cookie, pages, v;
	void *a;
	int bad = 0;

	printf("\n[A] alias nents=1 (4 pages)\n");
	if (alloc_src(fd, STRIDE, 0xaaaaaaaa11111111ULL, &src))
		return;
	if (make_alias(fd, &src, 1, STRIDE, &cookie, &pages))
		return;
	a = mmap(NULL, pages * PAGE, PROT_READ, MAP_SHARED, fd, (off_t)cookie);
	if (a == MAP_FAILED) {
		printf("  full mmap failed: errno=%d (%s)\n", errno, strerror(errno));
		return;
	}
	printf("  full va=%p\n", a);
	for (uint64_t pg = 0; pg < STRIDE; pg++) {
		if (guarded_read64((volatile uint64_t *)((char *)a + pg * PAGE), &v)) {
			printf("  page[%llu] SIGBUS at 0x%llx\n",
			       (unsigned long long)pg, (unsigned long long)fault_addr);
			bad++;
		} else {
			printf("  page[%llu] = 0x%llx %s\n", (unsigned long long)pg,
			       (unsigned long long)v,
			       v == 0xaaaaaaaa11111111ULL ? "(match)" : "(MISMATCH)");
		}
	}
	printf("  result: %s\n", bad ? "FAULTS" : "all 4 pages readable");
	munmap(a, pages * PAGE);
}

/* nents=2: full VMA only page 0 of chunk 0 is readable; chunks re-mapped. */
static void test_two(int fd)
{
	uint64_t src[2], cookie, pages, v;
	void *a, *c[2];

	printf("\n[B] alias nents=2 (2 x 4 pages, stride 4)\n");
	if (alloc_src(fd, STRIDE, 0xaaaabbbbccccddddULL, &src[0]))
		return;
	if (alloc_src(fd, STRIDE, 0x1111222233334444ULL, &src[1]))
		return;
	if (make_alias(fd, src, 2, STRIDE, &cookie, &pages))
		return;
	a = mmap(NULL, pages * PAGE, PROT_READ, MAP_SHARED, fd, (off_t)cookie);
	if (a == MAP_FAILED) {
		printf("  full mmap failed: errno=%d (%s)\n", errno, strerror(errno));
		return;
	}
	printf("  full va=%p (8 pages)\n", a);
	if (guarded_read64((volatile uint64_t *)a, &v) == 0)
		printf("  full[page0] = 0x%llx %s\n", (unsigned long long)v,
		       v == 0xaaaabbbbccccddddULL ? "(match)" : "(MISMATCH)");
	else
		printf("  full[page0] SIGBUS at 0x%llx (unexpected)\n",
		       (unsigned long long)fault_addr);

	/* Never read page >= STRIDE from the full VMA: kernel returns SIGBUS. */
	printf("  (skipping pages 4..7 of the full VMA: SIGBUS by design)\n");

	/* Reproduce the exploit's per-chunk re-mmap. */
	for (int i = 0; i < 2; i++) {
		c[i] = mmap(NULL, STRIDE * PAGE, PROT_READ, MAP_SHARED, fd,
			    (off_t)((uint64_t)(uintptr_t)a + i * STRIDE * PAGE));
		if (c[i] == MAP_FAILED) {
			printf("  chunk%d mmap failed: errno=%d (%s)\n", i, errno,
			       strerror(errno));
			continue;
		}
		printf("  chunk%d va=%p\n", i, c[i]);
		if (guarded_read64((volatile uint64_t *)c[i], &v) == 0)
			printf("  chunk%d[page0] = 0x%llx %s\n", i,
			       (unsigned long long)v,
			       v == (i ? 0x1111222233334444ULL
				       : 0xaaaabbbbccccddddULL)
				       ? "(match)" : "(MISMATCH)");
		else
			printf("  chunk%d[page0] SIGBUS at 0x%llx\n", i,
			       (unsigned long long)fault_addr);
	}
	for (int i = 0; i < 2; i++)
		if (c[i] != MAP_FAILED)
			munmap(c[i], STRIDE * PAGE);
	munmap(a, pages * PAGE);
}

int main(void)
{
	struct sigaction sa;
	int fd;

	setbuf(stdout, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = fault_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);

	printf("== HLK-AL00 isolated MEM_ALIAS probe ==\n");
	fd = open_mali();
	if (fd < 0)
		return 1;

	test_single(fd);
	test_two(fd);

	close(fd);
	printf("\nalias probe done, still alive\n");
	return 0;
}