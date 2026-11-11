/* ION phase-3: verify secure-buffer mmap is refused (statics say -EINVAL, no panic path).
 * Safe by construction: alloc from secure heap, attempt mmap, close fd, exit.
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

struct ion_allocation_data {
	uint64_t len;
	uint32_t heap_id_mask;
	uint32_t flags;
	uint32_t fd;
	uint32_t unused;
};

struct ion_heap_data {
	uint32_t id;
	uint32_t type;
	char name[16];
};

struct ion_heap_query {
	uint32_t cnt;
	uint32_t reserved0;
	uint64_t heaps;
	uint32_t reserved1;
	uint32_t reserved2;
};

#define _MY_IOC(d, t, nr, sz) (((d) << 30) | ((t) << 8) | (nr) | ((sz) << 16))
#define ION_IOC_ALLOC   _MY_IOC(3, 'I', 0, 24)
#define ION_IOC_HEAP_QUERY _MY_IOC(3, 'I', 8, 24)

#define FLAG_SECURE (1u << 3)

static int do_alloc(int fd, uint32_t mask, uint32_t flags, size_t len, int *out_fd)
{
	struct ion_allocation_data d;
	memset(&d, 0, sizeof(d));
	d.len = len;
	d.heap_id_mask = mask;
	d.flags = flags;
	if (ioctl(fd, ION_IOC_ALLOC, &d) < 0)
		return -1;
	*out_fd = (int)d.fd;
	return 0;
}

static void try_mmap(const char *label, int dmabuf, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf, 0);
	if (p == MAP_FAILED) {
		printf("%-28s mmap refused: errno=%d (%s)\n", label, errno, strerror(errno));
	} else {
		/* Statics say this branch is unreachable for secure heaps. If we
		 * ever get here, do NOT dereference; just report and unmap. */
		printf("%-28s mmap SUCCEEDED p=%p (unexpected!)\n", label, p);
		munmap(p, len);
	}
}

int main(void)
{
	int fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		printf("open /dev/ion: %s\n", strerror(errno));
		return 1;
	}

	struct {
		struct ion_heap_query q;
		struct ion_heap_data h[32];
	} qbuf;
	memset(&qbuf, 0, sizeof(qbuf));
	qbuf.q.cnt = 32;
	qbuf.q.heaps = (uint64_t)(uintptr_t)qbuf.h;
	if (ioctl(fd, ION_IOC_HEAP_QUERY, &qbuf.q) == 0) {
		for (uint32_t i = 0; i < qbuf.q.cnt && i < 32; i++)
			printf("heap[%u] id=%u type=%u name=%s\n",
			       i, qbuf.h[i].id, qbuf.h[i].type, qbuf.h[i].name);
	}

	/* 1) SEC_CONTIG secure buffer (protect_heap, id 20): mmap must fail */
	int bfd = -1;
	if (do_alloc(fd, 1u << 20, FLAG_SECURE, 256 * 1024, &bfd) == 0) {
		printf("protect_heap(20,SEC) alloc ok fd=%d\n", bfd);
		try_mmap("sec_contig(20)", bfd, 256 * 1024);
		close(bfd);
	} else {
		printf("protect_heap(20,SEC) alloc failed: %s\n", strerror(errno));
	}

	/* 2) CPA secure buffer (protect_cpa_heap, id 22): no map_user at all */
	if (do_alloc(fd, 1u << 22, FLAG_SECURE, 256 * 1024, &bfd) == 0) {
		printf("protect_cpa(22,SEC) alloc ok fd=%d\n", bfd);
		try_mmap("cpa(22)", bfd, 256 * 1024);
		close(bfd);
	} else {
		printf("protect_cpa(22,SEC) alloc failed: %s\n", strerror(errno));
	}

	/* 3) sanity: normal buffer still mmaps fine */
	if (do_alloc(fd, 1u << 0, 0, 4096, &bfd) == 0) {
		printf("system(0) alloc ok fd=%d\n", bfd);
		try_mmap("system(0)", bfd, 4096);
		close(bfd);
	} else {
		printf("system(0) alloc failed: %s\n", strerror(errno));
	}

	close(fd);
	printf("done, still alive\n");
	return 0;
}
