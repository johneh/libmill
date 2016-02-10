#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include "libmill.h"
#include "cr.h"
#include "utils.h"
#include "task.h"

static void *zmalloc(size_t sz);
static void zfree(void *ptr, size_t sz);
void *(*mmalloc)(size_t size) = zmalloc;  
void (*mfree)(void *ptr, size_t size) = zfree;

enum task_code {
    tTASK = 1,
    tSTAT,
    tOPEN,
    tCLOSE,
    tPREAD,
    tPWRITE,
    tUNLINK,
    tREADV,
    tWRITEV,
};

static tchan mill_tasks;  /* global task queue */

typedef struct {
    enum task_code code;
    void *cr;   /* task coroutine */

    /* input and/or output */
    void *buf;
    /* input */
    union {
        struct {
            char *path;
            int flags;
            mode_t mode;
        };
        struct {
            int fd;
            size_t count;
            off_t offset;
        };
        fn_task task;
    };

    /* output */
    int errcode;    /* errno */
    union {
        int ofd;
        ssize_t ssz;
    };
    int res_fd; /* tell worker where to send the response */
} a_task;


static void *zmalloc(size_t sz) {
    void *ptr = malloc(sz);
    if (! ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return ptr;
}

static void zfree(void *ptr, size_t size) {
    free(ptr);
}

coroutine static void wait_task(int fd) {
    unsigned size = sizeof(a_task *);
    a_task *res;
    while (1) {
        int n = (int) read(fd, (void *) & res, size);
        if (n == size) {
            mill->num_tasks--;
            mill_resume(res->cr, 1);
            continue;
        }
        mill_assert(n < 0);
        if (errno == EINTR)
            continue;
        /* EAGAIN -- pipe capacity execeeded ? */
        mill_assert(errno == EAGAIN);
        fdwait(fd, FDW_IN, -1);
    }
}

static ssize_t queue_task(a_task *req) {
    mill_assert(mill != NULL);
    /* Using pipe for notification of finished tasks */
    if (mill->tasks_fd[0] == -1) {
        if (-1 == pipe(mill->tasks_fd)) {
            mfree(req, sizeof (a_task));
            return -1;
        }
        int flag = fcntl(mill->tasks_fd[0], F_GETFL);
        if (flag == -1)
            flag = 0;
        (void) fcntl(mill->tasks_fd[0], F_SETFL, flag|O_NONBLOCK);
        flag = fcntl(mill->tasks_fd[1], F_GETFL);
        if (flag == -1)
            flag = 0;
        (void) fcntl(mill->tasks_fd[1], F_SETFL, flag|O_NONBLOCK);
        go(wait_task(mill->tasks_fd[0]));
    }

    req->res_fd = mill->tasks_fd[1];
    req->cr = mill->running;
    tchs(mill_tasks, a_task *, req, NULL);
    mill->num_tasks++;
    (void) mill_suspend();
    req->cr = NULL;

    int errcode = req->errcode;
    ssize_t ret = 0;
    switch (req->code) {
    case tOPEN:
        ret = req->ofd;
        break;
    case tPREAD: case tPWRITE: case tREADV: case tWRITEV:
    case tTASK:
        ret = req->ssz;
        break;
    case tSTAT: case tUNLINK:
        /* fall through */
    default:
        if (errcode)
            ret = -1;
    }

    mfree(req, sizeof (a_task));
    errno = errcode;
    return ret;
}

int stat_a(const char *path, struct stat *buf) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tSTAT;
    req->path = (char *) path;
    req->buf = (void *) buf;
    return queue_task(req);
}

int open_a(const char *path, int flags, mode_t mode) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tOPEN;
    req->path = (char *) path;
    req->flags = flags;
    req->mode = mode;
    return queue_task(req);
}

int close_a(int fd) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tCLOSE;
    req->fd = fd;
    return queue_task(req);
}

ssize_t pread_a(int fd, void *buf, size_t count, off_t offset) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tPREAD;
    req->fd = fd;
    req->count = count;
    req->offset = offset;
    req->buf = (void *) buf;
    return queue_task(req);
}

ssize_t pwrite_a(int fd, const void *buf, size_t count, off_t offset) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tPWRITE;
    req->fd = fd;
    req->buf = (void *) buf;
    req->count = count;
    req->offset = offset;
    return queue_task(req);
}

int unlink_a(const char *path) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tUNLINK;
    req->path = (char *) path;
    return queue_task(req);
}

ssize_t readv_a(int fd, const struct iovec *iov, int iovcnt) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tREADV;
    req->fd = fd;
    req->buf = (void *) iov;
    req->count = iovcnt;
    return queue_task(req);
}

ssize_t writev_a(int fd, const struct iovec *iov, int iovcnt) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tWRITEV;
    req->fd = fd;
    req->buf = (void *) iov;
    req->count = iovcnt;
    return queue_task(req);
}

#if 0
int task_a(fn_task tf, void *da) {
    a_task *req = mmalloc(sizeof (a_task));
    req->code = tTASK;
    req->task = tf;
    req->buf = da;
    return queue_task(req);
}
#endif

static int signal_task(int fd, a_task *res) {
    int size = sizeof (a_task *);
    while (1) {
        int n = (int) write(fd, (void *) & res, size);
        if (n == size)
            break;
        mill_assert(n < 0);
        if (errno == EINTR)
            continue;
        /* EAGAIN -- pipe capacity execeeded ? */
        if (errno != EAGAIN)
            return -1;
        fdwait(fd, FDW_OUT, -1);
    }
    return 0;
}

static void *worker_func(void *p) {
    tchan chreq = p;
    int done = 0;

    mill_init();
    while (1) {
        a_task *req = tchr(chreq, a_task *, &done);
        if (done)   /* XXX: really ? */
            break;
        a_task *res = req;
        res->errcode = 0;
        switch (req->code) {
        case tSTAT:
            if (-1 == stat(req->path, (struct stat *) req->buf))
                res->errcode = errno;
            break;
        case tOPEN:
            res->ofd = open(req->path, req->flags, req->mode);
            if (-1 == req->ofd)
                res->errcode = errno;
            break;
        case tCLOSE:
            if (-1 == close(req->fd))
                res->errcode = errno;
            break;
        case tPREAD:
            res->ssz = pread(req->fd, req->buf, req->count, req->offset);
            if (-1 == res->ssz)
                res->errcode = errno;
            break;
        case tPWRITE:
            res->ssz = pwrite(req->fd, req->buf, req->count, req->offset);
            if (-1 == res->ssz)
                res->errcode = errno;
            break;
        case tUNLINK:
            if (-1 == unlink(req->path))
                res->errcode = errno;
            break;
        case tREADV:
            res->ssz = readv(req->fd, req->buf, req->count);
            if (-1 == res->ssz)
                res->errcode = errno;
            break;
        case tWRITEV:
            res->ssz = writev(req->fd, req->buf, req->count);
            if (-1 == res->ssz)
                res->errcode = errno;
            break;
        case tTASK:
            req->task(req->buf);
            req->ssz = 0;
            break;
        default:
            mill_panic("worker_func(): received unexpected code");
        }
        if (-1 == signal_task(req->res_fd, res))
            mfree(res, sizeof (a_task));
    }

    mill_free();
    return NULL;
}

struct worker {
    pthread_t pth;
    tchan req_ch;
};

#ifndef NUM_WORKER
#define NUM_WORKER 2
#endif
static struct worker workers[NUM_WORKER];

void init_workers(void) {
    int i, rc;
    pthread_attr_t attr;

    mill_tasks = tchmake(sizeof (a_task *));
    rc = pthread_attr_init(&attr);
    mill_assert(rc == 0);
    pthread_attr_setdetachstate(& attr, PTHREAD_CREATE_DETACHED);
    for (i = 0; i < NUM_WORKER; i++) {
        workers[i].req_ch = tchdup(mill_tasks);
        rc = pthread_create(& workers[i].pth, & attr, worker_func, workers[i].req_ch);
        mill_assert(rc == 0);
    }
    pthread_attr_destroy(& attr);
}
