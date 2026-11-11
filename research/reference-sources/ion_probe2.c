/* ion_probe2.c — phase 2: secure heap allocation reachability from shell */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
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

#define ION_FLAG_CACHED          1u
#define ION_FLAG_SECURE_BUFFER   (1u << 3)

#define _MY_IOC(d, t, nr, sz) (((d) << 30) | ((sz & 0x3FFF) << 16) | ((t) << 8) | (nr))
#define ION_IOC_ALLOC _MY_IOC(3, ION_IOC_MAGIC, 0, sizeof(struct ion_allocation_data))

static void try_alloc(int fd, const char *label, uint32_t heap_bit,
		      uint32_t flags, uint64_t len)
{
	struct ion_allocation_data a;
	memset(&a, 0, sizeof(a));
	a.len = len;
	a.heap_id_mask = heap_bit;
	a.flags = flags;
	int ret = ioctl(fd, ION_IOC_ALLOC, &a);
	if (ret != 0) {
		printf("[-] %-22s -> FAIL errno=%d (%s)\n", label, errno,
		       strerror(errno));
		return;
	}
	printf("[+] %-22s -> OK dma-buf fd=%d (len=%llu flags=0x%x)\n",
	       label, a.fd, (unsigned long long)a.len, a.flags);
	/* do NOT mmap secure buffers; just close */
	close(a.fd);
}

int main(void)
{
	int fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/ion");
		return 1;
	}
	printf("[+] /dev/ion opened, testing heap reachability (256K allocs)\n");

	try_alloc(fd, "sys_heap(id0)", 1u << 0, ION_FLAG_CACHED, 256 * 1024);
	try_alloc(fd, "sec_heap(id5,SEC)", 1u << 5, ION_FLAG_SECURE_BUFFER,
		  256 * 1024);
	try_alloc(fd, "sec_heap(id5,noSEC)", 1u << 5, 0, 256 * 1024);
	try_alloc(fd, "normal_sync(id14)", 1u << 14, ION_FLAG_CACHED,
		  256 * 1024);
	try_alloc(fd, "camera_heap(id19)", 1u << 19, ION_FLAG_CACHED,
		  256 * 1024);
	try_alloc(fd, "protect(id20,SEC)", 1u << 20, ION_FLAG_SECURE_BUFFER,
		  256 * 1024);
	try_alloc(fd, "protect_cpa(id22,SEC)", 1u << 22, ION_FLAG_SECURE_BUFFER,
		  256 * 1024);
	try_alloc(fd, "multi_mask 5|20 SEC",
		  (1u << 5) | (1u << 20), ION_FLAG_SECURE_BUFFER, 256 * 1024);

	close(fd);
	printf("[+] phase2 done\n");
	return 0;
}
