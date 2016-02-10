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
    int errcode;    /* errno and TASK_status */
#define TASK_QUEUED     -1
#define TASK_TIMEDOUT   -2
#define TASK_INPROGRESS 0

    struct mill_cr *cr;   /* task coroutine */

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
        task_func taskfn;
    };

    /* output */
    union {
        int ofd;
        ssize_t ssz;
    };
    int res_fd; /* tell worker where to send the response */
} task_s;


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

static void mill_task_timedout(struct mill_timer *timer) {
    struct mill_cr *cr = mill_cont(timer, struct mill_cr, timer);
    mill_resume(cr, -1);
}

coroutine static void wait_task(int fd) {
    unsigned size = sizeof(task_s *);
    task_s *res;
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

static ssize_t queue_task(task_s *req, int64_t deadline) {
    mill_assert(mill != NULL);
    /* Using pipe for notification of finished tasks */
    if (mill->tasks_fd[0] == -1) {
        if (-1 == pipe(mill->tasks_fd)) {
            mfree(req, sizeof (task_s));
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

    req->errcode = TASK_QUEUED;
    req->res_fd = mill->tasks_fd[1];
    req->cr = mill->running;
    tchs(mill_tasks, task_s *, req, NULL);
    mill->num_tasks++;

    if(deadline >= 0)
        mill_timer_add(&mill->running->timer, deadline, mill_task_timedout);

    if (-1 == mill_suspend()) {
        if (__sync_bool_compare_and_swap(&req->errcode, TASK_QUEUED, TASK_TIMEDOUT)) {
            mill->num_tasks--;
            errno = ETIMEDOUT;
            /* task struct will be freed by the worker */
            return -1;
        }
        /* TASK_INPROGRESS */
        (void) mill_suspend();
    } else if (deadline >= 0)
        mill_timer_rm(&req->cr->timer);

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

    mfree(req, sizeof (task_s));
    errno = errcode;
    return ret;
}

int stat_a(const char *path, struct stat *buf) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tSTAT;
    req->path = (char *) path;
    req->buf = (void *) buf;
    return queue_task(req, -1);
}

int open_a(const char *path, int flags, mode_t mode) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tOPEN;
    req->path = (char *) path;
    req->flags = flags;
    req->mode = mode;
    return queue_task(req, -1);
}

int close_a(int fd) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tCLOSE;
    req->fd = fd;
    return queue_task(req, -1);
}

ssize_t pread_a(int fd, void *buf, size_t count, off_t offset) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tPREAD;
    req->fd = fd;
    req->count = count;
    req->offset = offset;
    req->buf = (void *) buf;
    return queue_task(req, -1);
}

ssize_t pwrite_a(int fd, const void *buf, size_t count, off_t offset) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tPWRITE;
    req->fd = fd;
    req->buf = (void *) buf;
    req->count = count;
    req->offset = offset;
    return queue_task(req, -1);
}

int unlink_a(const char *path) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tUNLINK;
    req->path = (char *) path;
    return queue_task(req, -1);
}

ssize_t readv_a(int fd, const struct iovec *iov, int iovcnt) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tREADV;
    req->fd = fd;
    req->buf = (void *) iov;
    req->count = iovcnt;
    return queue_task(req, -1);
}

ssize_t writev_a(int fd, const struct iovec *iov, int iovcnt) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tWRITEV;
    req->fd = fd;
    req->buf = (void *) iov;
    req->count = iovcnt;
    return queue_task(req, -1);
}


int task_run(task_func tf, void *data, int64_t deadline) {
    task_s *req = mmalloc(sizeof (task_s));
    req->code = tTASK;
    req->taskfn = tf;
    req->buf = data;
    return queue_task(req, deadline);
}

static int signal_task(int fd, task_s *res) {
    int size = sizeof (task_s *);
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
    while (! done) {
        task_s *req = tchr(chreq, task_s *, &done);
        if (done)   /* Unlikely */
            goto x;
        if (! __sync_bool_compare_and_swap(&req->errcode,
                        TASK_QUEUED, TASK_INPROGRESS)
        ) {
            /* errcode == TASK_TIMEDOUT */
            mfree(req, sizeof (task_s));
            goto x;
        }

        req->errcode = 0;
        switch (req->code) {
        case tSTAT:
            if (-1 == stat(req->path, (struct stat *) req->buf))
                req->errcode = errno;
            break;
        case tOPEN:
            req->ofd = open(req->path, req->flags, req->mode);
            if (-1 == req->ofd)
                req->errcode = errno;
            break;
        case tCLOSE:
            if (-1 == close(req->fd))
                req->errcode = errno;
            break;
        case tPREAD:
            req->ssz = pread(req->fd, req->buf, req->count, req->offset);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tPWRITE:
            req->ssz = pwrite(req->fd, req->buf, req->count, req->offset);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tUNLINK:
            if (-1 == unlink(req->path))
                req->errcode = errno;
            break;
        case tREADV:
            req->ssz = readv(req->fd, req->buf, req->count);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tWRITEV:
            req->ssz = writev(req->fd, req->buf, req->count);
            if (-1 == req->ssz)
                req->errcode = errno;
            break;
        case tTASK:
            req->taskfn(req->buf);
            req->ssz = 0;
            break;
        default:
            mill_panic("worker_func(): received unexpected code");
        }
        if (-1 == signal_task(req->res_fd, req))
            mfree(req, sizeof (task_s));
x:;
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

    mill_tasks = tchmake(sizeof (task_s *));
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
