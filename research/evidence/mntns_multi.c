#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define DIR "/dev/hlk-ns-m"
#define MA  DIR "/a"
#define MB  DIR "/b"

struct rep { int id; int rc; int sees_a, sees_b; char nsid[96]; };

static void nsid(char *o, size_t n) {
	ssize_t k = readlink("/proc/self/ns/mnt", o, n - 1);
	if (k < 0) { snprintf(o, n, "ERR"); return; }
	o[k] = 0;
}
static int seen(const char *mp) {
	FILE *f = fopen("/proc/self/mounts", "r");
	if (!f) return -1;
	char l[512]; int c = 0;
	while (fgets(l, sizeof l, f)) if (strstr(l, mp)) c++;
	fclose(f); return c;
}
int main(void) {
	int rp[2], go[2];
	pipe(rp); pipe(go);
	char pn[96]; nsid(pn, sizeof pn);
	printf("PARENT ns: %s\n", pn);
	mkdir(DIR, 0755); mkdir(MA, 0755); mkdir(MB, 0755);

	pid_t p1 = fork();
	if (p1 == 0) {
		struct rep r; memset(&r, 0, sizeof r);
		r.id = 1;
		close(rp[0]); close(go[1]);
		r.rc = unshare(CLONE_NEWNS);
		if (!r.rc) {
			mount("none", "/", NULL, MS_PRIVATE | MS_REC, NULL);
			mount("tmpfs", MA, "tmpfs", 0, NULL);
			nsid(r.nsid, sizeof r.nsid);
		}
		char ready = 'R'; write(rp[1], &ready, 1);
		char g; read(go[0], &g, 1);
		r.sees_a = seen(MA); r.sees_b = seen(MB);
		write(rp[1], &r, sizeof r);
		_exit(0);
	}
	pid_t p2 = fork();
	if (p2 == 0) {
		struct rep r; memset(&r, 0, sizeof r);
		r.id = 2;
		close(rp[0]); close(go[1]);
		r.rc = unshare(CLONE_NEWNS);
		if (!r.rc) {
			mount("none", "/", NULL, MS_PRIVATE | MS_REC, NULL);
			mount("tmpfs", MB, "tmpfs", 0, NULL);
			nsid(r.nsid, sizeof r.nsid);
		}
		char ready = 'R'; write(rp[1], &ready, 1);
		char g; read(go[0], &g, 1);
		r.sees_a = seen(MA); r.sees_b = seen(MB);
		write(rp[1], &r, sizeof r);
		_exit(0);
	}
	close(rp[1]); close(go[0]);
	char r1, r2; read(rp[0], &r1, 1); read(rp[0], &r2, 1);
	printf("both children ready, PARENT sees: a=%d b=%d\n", seen(MA), seen(MB));
	write(go[1], "GG", 2);
	struct rep x, y; memset(&x, 0, sizeof x); memset(&y, 0, sizeof y);
	read(rp[0], &x, sizeof x); read(rp[0], &y, sizeof y);
	struct rep a = (x.id == 1) ? x : y, b = (x.id == 1) ? y : x;
	printf("child A (id=%d, mounted a): unshare rc=%d ns=%s  sees a=%d b=%d\n", a.id, a.rc, a.nsid, a.sees_a, a.sees_b);
	printf("child B (id=%d, mounted b): unshare rc=%d ns=%s  sees a=%d b=%d\n", b.id, b.rc, b.nsid, b.sees_a, b.sees_b);
	int st; waitpid(p1, &st, 0); waitpid(p2, &st, 0);
	printf("after exit PARENT sees: a=%d b=%d\n", seen(MA), seen(MB));
	int i; for (i = 0; i < 4 && seen(MA) > 0; i++) umount(MA);
	for (i = 0; i < 4 && seen(MB) > 0; i++) umount(MB);
	rmdir(MA); rmdir(MB); rmdir(DIR);
	printf("cleaned, FINAL: a=%d b=%d\n", seen(MA), seen(MB));
	return 0;
}
