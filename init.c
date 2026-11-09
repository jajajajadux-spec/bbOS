#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void try_mount(const char *src, const char *target, const char *type) {
    if (mkdir(target, 0755) < 0 && errno != EEXIST) perror(target);
    if (mount(src, target, type, 0, NULL) < 0 && errno != EBUSY)
        perror(target);
}

int main(void) {
    /* Refuse to change the host's mounts during an accidental test run. */
    if (getpid() != 1) {
        fputs("bbOS init must run as PID 1\n", stderr);
        return 1;
    }
    try_mount("proc", "/proc", "proc");
    try_mount("sysfs", "/sys", "sysfs");
    try_mount("devtmpfs", "/dev", "devtmpfs");
    if (mount(NULL, "/", NULL, MS_REMOUNT, NULL) < 0) perror("remount /");
    if (setenv("PATH", "/usr/local/sbin:/usr/local/bin:/sbin:/usr/sbin:/bin:/usr/bin", 1) < 0)
        perror("setenv");
    puts("\nBurntballOS (bbOS) -- PID 1 online. Starting a shell...\n");
    fflush(NULL);
    for (;;) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            sleep(1);
            continue;
        }
        if (pid == 0) {
            if (setsid() < 0) perror("setsid");
            int fd = open("/dev/console", O_RDWR);
            if (fd < 0) { perror("/dev/console"); _exit(1); }
            if (ioctl(fd, TIOCSCTTY, 0) < 0) perror("TIOCSCTTY");
            for (int i = 0; i < 3; ++i)
                if (dup2(fd, i) < 0) { perror("dup2"); _exit(1); }
            if (fd > 2) close(fd);
            execl("/bin/sh", "sh", (char *)NULL);
            perror("/bin/sh");
            _exit(127);
        }
        for (;;) {
            pid_t reaped = waitpid(-1, NULL, 0);
            if (reaped == pid) break;
            if (reaped < 0 && errno != EINTR) {
                perror("waitpid");
                break;
            }
        }
        /* Bound retries even when the shell is missing or exits immediately. */
        fputs("bbOS init: shell exited; restarting in one second\n", stderr);
        sleep(1);
    }
}
