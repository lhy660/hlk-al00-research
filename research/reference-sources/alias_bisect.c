/* HLK-AL00 MEM_ALIAS bisection probe.
 *
 * The isolated alias probe panicked. To find the exact step even if the pty
 * output is lost when the kernel dies, every step is appended to
 * /data/local/tmp/alias_bisect.log and fsync()ed before the next step runs.
 *
 * Usage: alias_bisect <mode>
 *   0  alloc one 4-page source (mmap+write), exit            (control)
 *   1  source + MEM_ALIAS ioctl, no alias mmap
 *   2  + alias mmap PROT_READ, no access
 *   3  + read alias page 0
 *   4  + read alias pages 0..3
 *   5  two sources + one nents=2 alias, full mmap, read page 0,
 *      then per-chunk re-mmap like the public exploit
 *
 * Build:
 *   tools/zig-windows-x86_64-0.11.0/zig.exe cc -target aarch64-linux-musl \
 *     -static -O2 -o probe/alias_bisect probe/alias_bisect.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
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
#define STRIDE 4ull
#define SPRAY_PAGES 25ull
#define LOGFILE "/data/local/tmp/alias_bisect.log"

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

	/* pty */
	write(1, buf, n);

	/* persistent, fsynced */
	fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		write(fd, buf, n);
		fsync(fd);
		close(fd);
	}
}

static int open_mali(void)
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
	mark("version %u.%u", vc.major, vc.minor);

	memset(&sf, 0, sizeof(sf));
	if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf) != 0) {
		mark("SET_FLAGS failed: %s", strerror(errno));
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
	mark("tracking page at %p", tracking);
	return fd;
}

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
		mark("MEM_ALLOC(%llu) failed errno=%d (%s)",
		     (unsigned long long)pages, errno, strerror(errno));
		return -1;
	}
	mark("alloc: cookie=0x%llx", (unsigned long long)a.out.gpu_va);
	p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)a.out.gpu_va);
	if (p == MAP_FAILED) {
		mark("mmap src failed errno=%d (%s)", errno, strerror(errno));
		return -1;
	}
	mark("src mmap at %p (cookie was 0x%llx)", p,
	     (unsigned long long)a.out.gpu_va);
	for (uint64_t i = 0; i < pages; i++)
		((volatile uint64_t *)((char *)p + i * PAGE))[0] = pat;
	mark("src wrote %llu pages", (unsigned long long)pages);
	*va = (uint64_t)(uintptr_t)p;
	return 0;
}

static int make_alias(int fd, uint64_t *src, uint64_t nents, uint64_t len,
		      uint64_t stride, uint64_t *alias_cookie, uint64_t *alias_pages)
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
	al.in.stride = stride;
	al.in.nents = nents;
	al.in.aliasing_info = (uint64_t)(uintptr_t)ai;
	errno = 0;
	mark("MEM_ALIAS ioctl: nents=%llu stride=%llu len=%llu handle0=0x%llx",
	     (unsigned long long)nents, (unsigned long long)stride,
	     (unsigned long long)len, (unsigned long long)src[0]);
	if (ioctl(fd, KBASE_IOCTL_MEM_ALIAS, &al) != 0) {
		mark("MEM_ALIAS failed errno=%d (%s)", errno, strerror(errno));
		return -1;
	}
	mark("MEM_ALIAS ok: flags=0x%llx cookie=0x%llx pages=%llu",
	     (unsigned long long)al.out.flags,
	     (unsigned long long)al.out.gpu_va,
	     (unsigned long long)al.out.va_pages);
	*alias_cookie = al.out.gpu_va;
	*alias_pages = al.out.va_pages;
	return 0;
}

int main(int argc, char **argv)
{
	int mode = argc > 1 ? atoi(argv[1]) : 0;
	uint64_t src, cookie = 0, pages = 0, v = 0;
	void *a;

	if (getuid() == 0)
		mark("(running as root)"); /* not expected */

	mark("=== alias_bisect mode %d ===", mode);

	if (mode == 0) {
		int fd = open_mali();
		if (fd < 0)
			return 1;
		if (alloc_src(fd, STRIDE, 0xaaaa0000ULL, &src))
			return 1;
		mark("mode 0 done");
		close(fd);
		return 0;
	}

	{
		int fd = open_mali();
		if (fd < 0)
			return 1;

		if (mode == 5) {
			uint64_t s2[2];
			void *full, *ch[2];
			mark("alloc source 0");
			if (alloc_src(fd, STRIDE, 0xaaaa0000ULL, &s2[0]))
				return 1;
			mark("alloc source 1");
			if (alloc_src(fd, STRIDE, 0xbbbb0000ULL, &s2[1]))
				return 1;
			mark("two sources mapped, calling MEM_ALIAS nents=2");
			if (make_alias(fd, s2, 2, STRIDE, STRIDE, &cookie, &pages))
				return 1;
			mark("mmap alias full (%llu pages) PROT_READ",
			     (unsigned long long)pages);
			full = mmap(NULL, pages * PAGE, PROT_READ, MAP_SHARED,
				    fd, (off_t)cookie);
			if (full == MAP_FAILED) {
				mark("alias full mmap failed errno=%d (%s)",
				     errno, strerror(errno));
				return 1;
			}
			mark("alias full va=%p; reading page 0", full);
			v = *(volatile uint64_t *)full;
			mark("full page0 = 0x%llx", (unsigned long long)v);
			mark("per-chunk remaps");
			for (int i = 0; i < 2; i++) {
				ch[i] = mmap(NULL, STRIDE * PAGE, PROT_READ,
					     MAP_SHARED, fd,
					     (off_t)((uint64_t)(uintptr_t)full +
						     i * STRIDE * PAGE));
				if (ch[i] == MAP_FAILED) {
					mark("chunk%d mmap failed errno=%d (%s)", i,
					     errno, strerror(errno));
					continue;
				}
				mark("chunk%d va=%p", i, ch[i]);
				v = *(volatile uint64_t *)ch[i];
				mark("chunk%d page0 = 0x%llx", i,
				     (unsigned long long)v);
			}
			mark("mode 5 done, alive");
			close(fd);
			return 0;
		}

		if (mode == 7) {
			/* Workaround shape: one nents=1 alias per source region,
			 * always reached through the cookie mmap (nr_pages=0), so
			 * kbase_gmc_decompress_alloc() is never called with a
			 * non-zero count on the zero-length ALIAS pages array.
			 */
			uint64_t s[2] = {0}, ck[2], pg[2];
			void *av[2];
			uint64_t pat[2] = {0x1111aaaa0000ULL, 0x2222bbbb0000ULL};
			int bad = 0;

			for (int i = 0; i < 2; i++) {
				mark("mode7: alloc+map src %d (%llu pages)", i,
				     (unsigned long long)SPRAY_PAGES);
				if (alloc_src(fd, SPRAY_PAGES, pat[i], &s[i]))
					return 1;
			}
			for (int i = 0; i < 2; i++) {
				mark("mode7: MEM_ALIAS nents=1 src %d", i);
				if (make_alias(fd, &s[i], 1, SPRAY_PAGES,
					       SPRAY_PAGES, &ck[i], &pg[i]))
					return 1;
				mark("mode7: cookie mmap alias %d (%llu pages)", i,
				     (unsigned long long)pg[i]);
				av[i] = mmap(NULL, pg[i] * PAGE, PROT_READ,
					     MAP_SHARED, fd, (off_t)ck[i]);
				if (av[i] == MAP_FAILED) {
					mark("mode7: alias mmap %d failed errno=%d (%s)",
					     i, errno, strerror(errno));
					return 1;
				}
				mark("mode7: alias %d va=%p", i, av[i]);
			}
			for (int i = 0; i < 2; i++) {
				for (uint64_t p = 0; p < SPRAY_PAGES; p++) {
					uint64_t x = *(volatile uint64_t *)
						((char *)av[i] + p * PAGE);
					if (x != pat[i]) {
						bad++;
						mark("mode7: alias%d page%llu = 0x%llx MISMATCH",
						     i, (unsigned long long)p,
						     (unsigned long long)x);
					}
				}
				mark("mode7: alias %d read %llu pages", i,
				     (unsigned long long)SPRAY_PAGES);
			}
			/* Alias must see later source writes (same physical pages). */
			for (int i = 0; i < 2; i++) {
				uint64_t nv = 0xfeed0000ULL + i;
				*(volatile uint64_t *)s[i] = nv;
				uint64_t x = *(volatile uint64_t *)av[i];
				mark("mode7: src%d write 0x%llx -> alias%d 0x%llx %s",
				     i, (unsigned long long)nv, i,
				     (unsigned long long)x,
				     x == nv ? "(sync ok)" : "(NOT SYNCED)");
			}
			mark("mode7 done, alive, mismatches=%d", bad);
			close(fd);
			return 0;
		}

		mark("alloc source");
		if (alloc_src(fd, STRIDE, 0xaaaa0000ULL, &src))
			return 1;

		mark("calling MEM_ALIAS nents=1");
		if (make_alias(fd, &src, 1, STRIDE, STRIDE, &cookie, &pages))
			return 1;
		mark("alias ready: cookie=0x%llx pages=%llu",
		     (unsigned long long)cookie, (unsigned long long)pages);
		if (mode == 1) {
			mark("mode 1 done (no alias mmap), alive");
			close(fd);
			return 0;
		}

		mark("mmap alias full PROT_READ (%llu pages)",
		     (unsigned long long)pages);
		a = mmap(NULL, pages * PAGE, PROT_READ, MAP_SHARED, fd,
			 (off_t)cookie);
		if (a == MAP_FAILED) {
			mark("alias mmap failed errno=%d (%s)", errno,
			     strerror(errno));
			return 1;
		}
		mark("alias va=%p", a);
		if (mode == 2) {
			mark("mode 2 done (no access), alive");
			close(fd);
			return 0;
		}

		mark("reading alias page 0");
		v = *(volatile uint64_t *)a;
		mark("page0 = 0x%llx", (unsigned long long)v);
		if (mode == 3) {
			mark("mode 3 done, alive");
			close(fd);
			return 0;
		}

		for (uint64_t pg = 0; pg < pages; pg++) {
			mark("reading page %llu", (unsigned long long)pg);
			v = *(volatile uint64_t *)((char *)a + pg * PAGE);
			mark("page%llu = 0x%llx", (unsigned long long)pg,
			     (unsigned long long)v);
		}
		mark("mode 4 done, alive");
		close(fd);
	}

	return 0;
}
