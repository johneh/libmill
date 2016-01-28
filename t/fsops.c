#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "libmill.h"

coroutine static void dot(char c, int *done) {
    while (! *done) {
        printf("%c", c);
        msleep(now() + 5);
    }
    printf("\nBye!\n");
}

static void pr_stat(const char *path, struct stat *s, int errcode) {
    if (errcode != 0) {
        printf("\nstat_a: %s: error: %s\n", path, strerror(errcode));
    } else {
        printf("\nstat_a: %s: type = %s, size = %u\n", path,
                S_ISDIR(s->st_mode) ? "dir" :
                    S_ISREG(s->st_mode) ? "file" : "?",
            (unsigned) s->st_size);
    }
}

coroutine static void do_stat(const char *path) {
    struct stat sbuf;
    int rc = stat_a(path, &sbuf);
    pr_stat(path, &sbuf, rc == -1 ? errno : 0);
}

coroutine static void do_close(int fd) {
    int rc = close_a(fd);
    if (rc == -1)
        printf("\nclose_a: (fd = %d): error: %s\n", fd, strerror(errno));
    else
        printf("\nclose_a: (fd = %d): Success!\n", fd);
}

coroutine static void do_pread(int fd) {
    char buf[512];
    ssize_t sz = pread_a(fd, buf, 512, 0);
    if (sz == -1)
        printf("\npread_a (fd = %d): error: %s\n", fd, strerror(errno));
    else
        printf("\npread_a (fd = %d): read %d bytes\n", fd, (int) sz);
    go(do_close(fd));
}

coroutine static void do_open(const char *path, int flags) {
    int fd = open_a(path, flags, 0666);
    if (fd == -1)
        printf("\nopen_a: %s: error: %s\n", path, strerror(errno));
    else {
        printf("\nopen_a: %s: fd = %d\n", path, fd);
        go(do_pread(fd));
    }
}

coroutine static void do_unlink(void) {
    char foo_path[] = "./_foo_bar_";
    struct stat sbuf;
    int rc = stat_a(foo_path, &sbuf);
    if (rc == 0) {
        fprintf(stderr, "do_unlink: path %s already exists\n", foo_path);
        return;
    }

    int fd = open_a(foo_path, O_CREAT, 0666);
    if (fd == -1)
        printf("\nopen_a: %s: error: %s\n", foo_path, strerror(errno));
    else {
        printf("\nopen_a: %s: fd = %d\n", foo_path, fd);
        int rc = unlink_a(foo_path);
        if (rc == -1)
            printf("\nunlink_a: %s (fd = %d): error: %s\n",
                        foo_path, fd, strerror(errno));
        else
            printf("\nunlink_a: %s (fd = %d): Success!\n", foo_path, fd);
        rc = close_a(fd);
        assert(rc == 0);
    }
}

coroutine static void do_pwrite(const char *source, const char *dest) {
    int infd = open_a(source, O_RDONLY, 0);
    int outfd = open_a(dest, O_CREAT|O_WRONLY, 0);

    assert(infd >= 0 && outfd >= 0);
    char buf[512];
    ssize_t sz = pread_a(infd, buf, 512, 0);
    if (sz == -1)
        printf("\npread_a (fd = %d): error: %s\n", infd, strerror(errno));
    else {
        sz = pwrite_a(outfd, (void *) buf, 512, 0);
        if (sz == -1)
            printf("\npwrite_a (fd = %d): error: %s\n", outfd, strerror(errno));
        else
            printf("\npwrite_a (fd = %d): wrote %d bytes\n", outfd, (int) sz);
    }
    int rc = close_a(infd);
    assert(rc == 0);
    rc = close_a(outfd);
    assert(rc == 0);
}

int main(void) {
    int done = 0;

    mill_init();
    go(dot('.', &done));
    go(do_open("../chan.c", O_RDONLY));
    go(do_stat("./_no_such_file_"));
    go(do_stat("../tutorial"));
    go(do_stat("../chan.c"));
    go(do_unlink());
    go(do_pwrite("/dev/zero", "/dev/null"));

    struct stat sbuf;
    int rc = stat_a("../cr.c", &sbuf);
    pr_stat("../cr.c", &sbuf, rc == -1 ? errno : 0);
#if 0
    done = 1;
    mill_waitfor();
#endif
    msleep(now() + 250);
    done = 1;
    msleep(now() + 25);
    mill_free();
    return 0;
}
