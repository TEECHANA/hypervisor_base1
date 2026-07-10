#include <unistd.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sys/ioctl.h>

int main(void) {
    /* BP0 — open /dev/console (static mknod'd node, no mounts needed) */
    int con = open("/dev/console", O_RDWR | O_NOCTTY);
    if (con < 0) con = open("/dev/ttyAMA0", O_RDWR | O_NOCTTY);

    if (con >= 0)
        write(con, "\r\n[BP0] init alive, console open OK\r\n", 37);
    /* If nothing prints here: init not running OR static /dev nodes broken */

    /* BP1 — proc */
    int r1 = mount("proc", "/proc", "proc", 0, 0);
    if (con >= 0)
        write(con, r1==0 ? "[BP1] proc OK\r\n" : "[BP1] proc FAIL\r\n", r1==0?15:17);

    /* BP2 — sysfs */
    int r2 = mount("sysfs", "/sys", "sysfs", 0, 0);
    if (con >= 0)
        write(con, r2==0 ? "[BP2] sysfs OK\r\n" : "[BP2] sysfs FAIL\r\n", r2==0?16:18);

    /* BP3 — devtmpfs (may overwrite static /dev nodes!) */
    int r3 = mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    if (con >= 0)
        write(con, r3==0 ? "[BP3] devtmpfs OK\r\n" : "[BP3] devtmpfs FAIL\r\n", r3==0?19:21);

    /* BP4 — dup console to stdin/stdout/stderr */
    dup2(con, 0); dup2(con, 1); dup2(con, 2);
    if (con > 2) close(con);
    write(1, "[BP4] stdio wired\r\n", 19);

    /* BP5 — open ttyAMA0 for proper TTY (after devtmpfs) */
    int tty = open("/dev/ttyAMA0", O_RDWR);
    if (tty >= 0) {
        write(1, "[BP5] ttyAMA0 OK\r\n", 18);
        setsid();
        ioctl(tty, TIOCSCTTY, 1);
        dup2(tty, 0); dup2(tty, 1); dup2(tty, 2);
        if (tty > 2) close(tty);
    } else {
        write(1, "[BP5] ttyAMA0 FAIL\r\n", 20);
    }

    /* BP6 — execve */
    write(1, "[BP6] calling execve /bin/sh\r\n", 30);
    char *argv[] = { "/bin/sh", "-i", NULL };
    char *envp[] = { "HOME=/", "PATH=/bin:/sbin", "TERM=vt100", "PS1=/ # ", NULL };
    execve("/bin/sh", argv, envp);

    /* BP7 — execve returned = failed */
    write(1, "[BP7] execve FAILED, hanging\r\n", 30);
    while(1) {}
    return 0;
}
