/* ion_probe.c — HLK-AL00 /dev/ion dynamic recon (phase 1: query + safe alloc) */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define ION_IOC_MAGIC 'I'

struct ion_allocation_data {
	uint64_t len;
	uint32_t heap_id_mask;
	uint32_t flags;
	uint32_t fd;
	uint32_t unused;
};

#define MAX_HEAP_NAME 32
struct ion_heap_data {
	char name[MAX_HEAP_NAME];
	uint32_t type;
	uint32_t heap_id;
	uint32_t reserved0;
	uint32_t reserved1;
	uint32_t reserved2;
};

struct ion_heap_query {
	uint32_t cnt;
	uint32_t reserved0;
	uint64_t heaps;
	uint32_t reserved1;
	uint32_t reserved2;
};

#define _MY_IOC(d, t, nr, sz) (((d) << 30) | ((sz & 0x3FFF) << 16) | ((t) << 8) | (nr))
#define ION_IOC_ALLOC       _MY_IOC(3, ION_IOC_MAGIC, 0, sizeof(struct ion_allocation_data))
#define ION_IOC_HEAP_QUERY  _MY_IOC(3, ION_IOC_MAGIC, 8, sizeof(struct ion_heap_query))

int main(void)
{
	int fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/ion");
		return 1;
	}
	printf("[+] /dev/ion opened (O_RDONLY), fd=%d\n", fd);

	/* step 1: query heap count */
	struct ion_heap_query q;
	memset(&q, 0, sizeof(q));
	if (ioctl(fd, ION_IOC_HEAP_QUERY, &q) != 0) {
		perror("HEAP_QUERY(count)");
		return 1;
	}
	printf("[+] registered heap count = %u\n", q.cnt);

	/* step 2: dump heaps */
	if (q.cnt > 0 && q.cnt <= 64) {
		struct ion_heap_data heaps[64];
		memset(heaps, 0, sizeof(heaps));
		memset(&q, 0, sizeof(q));
		q.cnt = 64;
		q.heaps = (uint64_t)(uintptr_t)heaps;
		if (ioctl(fd, ION_IOC_HEAP_QUERY, &q) != 0) {
			perror("HEAP_QUERY(dump)");
			return 1;
		}
		for (uint32_t i = 0; i < q.cnt; i++)
			printf("    heap[%2u] name=%-16s type=%u id=%u\n",
			       i, heaps[i].name, heaps[i].type, heaps[i].heap_id);
	}

	/* step 3: safe allocation, system heap, 64KB */
	struct ion_allocation_data alloc;
	memset(&alloc, 0, sizeof(alloc));
	alloc.len = 64 * 1024;
	alloc.heap_id_mask = 1; /* ION_SYSTEM_HEAP_ID assumed 0 -> bit0 */
	alloc.flags = 1;        /* ION_FLAG_CACHED */
	if (ioctl(fd, ION_IOC_ALLOC, &alloc) != 0) {
		perror("ALLOC(system 64K)");
	} else {
		printf("[+] ALLOC ok, dma-buf fd=%d\n", alloc.fd);
		void *p = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED,
			       alloc.fd, 0);
		if (p == MAP_FAILED) {
			perror("mmap dma-buf");
		} else {
			memset(p, 0x41, 4096);
			printf("[+] mmap + write ok (%p)\n", p);
			munmap(p, 64 * 1024);
		}
		close(alloc.fd);
	}

	close(fd);
	printf("[+] phase1 done\n");
	return 0;
}
