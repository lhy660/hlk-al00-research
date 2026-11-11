#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MPA "/dev/hlk-ns-a"
#define MPB "/dev/hlk-ns-b"

struct res {
	int unshare_rc, unshare_errno;
	int priv_rc, priv_errno;
	int mount_rc, mount_errno;
	int child_sees;
	char nsid[96];
};

static void get_nsid(char *out, size_t n) {
	ssize_t k = readlink("/proc/self/ns/mnt", out, n - 1);
	if (k < 0) { snprintf(out, n, "ERR:%s", strerror(errno)); return; }
	out[k] = 0;
}

static int seen(const char *mp) {
	FILE *f = fopen("/proc/self/mounts", "r");
	if (!f) return -1;
	char line[512]; int c = 0;
	while (fgets(line, sizeof line, f))
		if (strstr(line, mp)) c++;
	fclose(f);
	return c;
}

static int run_case(const char *tag, const char *mp, int make_private) {
	int c2p[2], p2c[2];
	if (pipe(c2p) || pipe(p2c)) { perror("pipe"); return -1; }
	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return -1; }
	if (pid == 0) {
		struct res r; memset(&r, 0, sizeof r);
		close(c2p[0]); close(p2c[1]);
		r.unshare_rc = unshare(CLONE_NEWNS);
		r.unshare_errno = errno;
		if (r.unshare_rc == 0) {
			get_nsid(r.nsid, sizeof r.nsid);
			if (make_private) {
				r.priv_rc = mount("none", "/", NULL, MS_PRIVATE | MS_REC, NULL);
				r.priv_errno = errno;
			}
			r.mount_rc = mount("tmpfs", mp, "tmpfs", 0, NULL);
			r.mount_errno = errno;
			r.child_sees = seen(mp);
		}
		if (write(c2p[1], &r, sizeof r) != (ssize_t)sizeof r) _exit(2);
		char go; if (read(p2c[0], &go, 1) < 0) _exit(3);
		_exit(0);
	}
	close(c2p[1]); close(p2c[0]);
	struct res r; memset(&r, 0, sizeof r);
	ssize_t got = read(c2p[0], &r, sizeof r);
	int during = seen(mp);
	printf("=== case %s (MS_PRIVATE|MS_REC = %d) ===\n", tag, make_private);
	if (got != (ssize_t)sizeof r) {
		printf("  child report read failed (%zd)\n", got);
	} else {
		printf("  child unshare(CLONE_NEWNS)      rc=%d %s\n",
		       r.unshare_rc, r.unshare_rc ? strerror(r.unshare_errno) : "");
		if (!r.unshare_rc) {
			printf("  child mnt ns id                 %s\n", r.nsid);
			printf("  child MS_PRIVATE|MS_REC         rc=%d %s\n",
			       r.priv_rc, make_private ? (r.priv_rc ? strerror(r.priv_errno) : "") : "(skipped)");
			printf("  child mount tmpfs               rc=%d %s\n",
			       r.mount_rc, r.mount_rc ? strerror(r.mount_errno) : "");
			printf("  child sees own mount            %d\n", r.child_sees);
		}
	}
	printf("  PARENT sees mount while child   %d\n", during);
	if (write(p2c[1], "G", 1) < 0) perror("write go");
	int st; waitpid(pid, &st, 0);
	int after = seen(mp);
	printf("  PARENT sees mount after exit    %d\n", after);
	close(c2p[0]); close(p2c[1]);
	return after;
}

int main(void) {
	char pns[96]; get_nsid(pns, sizeof pns);
	printf("PARENT mnt ns: %s\n", pns);
	printf("initial: A=%d B=%d\n\n", seen(MPA), seen(MPB));
	mkdir(MPA, 0755); mkdir(MPB, 0755);
	int ra = run_case("A", MPA, 1);
	printf("\n");
	int rb = run_case("B", MPB, 0);
	printf("\n=== cleanup ===\n");
	int i, ua = 0, ub = 0;
	for (i = 0; i < 4 && seen(MPA) > 0; i++) { ua = umount(MPA); if (ua) break; }
	for (i = 0; i < 4 && seen(MPB) > 0; i++) { ub = umount(MPB); if (ub) break; }
	rmdir(MPA); rmdir(MPB);
	printf("umount A rc=%d  B rc=%d (residue reported: A=%d B=%d)\n", ua, ub, ra, rb);
	printf("FINAL parent sees: A=%d B=%d\n", seen(MPA), seen(MPB));
	return 0;
}
