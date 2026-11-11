#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* 触发 request_module("net-pf-4")：AF_IPX 必然未注册 */
int main(void)
{
	int fd = socket(AF_IPX, SOCK_DGRAM, 0);
	printf("socket(AF_IPX) fd=%d errno=%d(%s)\n", fd, errno, strerror(errno));
	if (fd >= 0) close(fd);
	return 0;
}
