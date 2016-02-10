#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include "libmill.h"
#include "utils.h"

/*
 * CAVEATS: tchmake(), tchdup() and tchclose() _should_ be called
 * in the same thread.
 */
  
struct channel_p {
    int fd[2];
    unsigned sz;
    int lockv;
    int done;
    int refcnt;
};

struct tchan_s {
    struct channel_p *ch;
    char valbuf[128];
};

tchan tchmake(unsigned sz) {
    int fd[2];
    struct channel_p *ch;
    tchan tch;
    int flag;

    mill_assert(sz <= 128);
    if (pipe(fd) == -1)
        return NULL;
    tch = malloc(sizeof (tchan));
    ch = malloc(sizeof(struct channel_p));
    if (! tch || ! ch) {
        close(fd[0]);
        close(fd[1]);
        errno = ENOMEM;
        return NULL;
    }
    flag = fcntl(fd[0], F_GETFL);
    if (flag != -1)
        fcntl(fd[0], F_SETFL, flag|O_NONBLOCK);
    flag = fcntl(fd[1], F_GETFL);
    if (flag != -1)
        fcntl(fd[1], F_SETFL, flag|O_NONBLOCK);
    ch->fd[0] = fd[0];
    ch->fd[1] = fd[1];
    ch->sz = sz;
    ch->lockv = 0;
    ch->done = 0;
    ch->refcnt = 1;
    tch->ch = ch;
    return tch;
}

tchan tchdup(tchan tch1) {
    tchan tch2 = malloc(sizeof (tchan));
    if (! tch2) {
        errno = ENOMEM;
        return NULL;
    }
    tch2->ch = tch1->ch;
    tch1->ch->refcnt++;
    return tch2;
}

void tchclose(tchan tch) {
    struct channel_p *ch = tch->ch;
    if (--ch->refcnt <= 0) {
        (void) close(ch->fd[1]);
        (void) close(ch->fd[0]);
        free(tch->ch);
    }
    free(tch);
}

static int trylock(struct channel_p *ch) {
    return __sync_bool_compare_and_swap(&ch->lockv, 0, 1);
}

static void unlock(struct channel_p *ch) {
    int ret = __sync_bool_compare_and_swap(&ch->lockv, 1, 0);
    mill_assert(ret);
}

static int pipe_read(struct channel_p *ch, void *ptr) {
    unsigned size = ch->sz;
    while (1) {
        if (trylock(ch)) {
            int n = (int) read(ch->fd[0], ptr,  size);
            unlock(ch);
            if (n == 0  /* closed -- done */
                || n == size
            ) {
                return n;
            }
            mill_assert(n < 0);
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN)
                return -1;
        }

        fdwait(ch->fd[0], FDW_IN, -1);

        /* more than one thread may receive notification,
         * race for the lock */
    }
}

static int pipe_write(struct channel_p *ch, void *ptr) {
    unsigned size = ch->sz;
    while (1) {
        int n = (int) write(ch->fd[1], ptr, ch->sz);
        if (n == size)
            break;
        mill_assert(n < 0);
        if (errno == EINTR)
            continue;
        /* EAGAIN -- pipe capacity execeeded ? */
        if (errno != EAGAIN)
            return -1;
        fdwait(ch->fd[1], FDW_OUT, -1);
    }
    return size;
}

void *tch_recv(tchan tch, size_t size, int *done) {
    mill_assert(size == tch->ch->sz);
    mill_assert(done);
    int rc = pipe_read(tch->ch, tch->valbuf);
    mill_assert(rc >= 0);
    *done = (rc == 0);
    return tch->valbuf;
}

void tch_send(tchan tch, void *ptr, size_t size, int *done) {
    mill_assert(size == tch->ch->sz);
    int rc = pipe_write(tch->ch, ptr);
    if (rc == -1 && tch->ch->done) {
        /* surely this is the reason -- print helpful error message */
        if (done) {
            *done = 1;
            return;
        }
        fprintf(stderr, "attempt to send to a closed 'tchan'\n");
        abort();
    }
    mill_assert(rc > 0);
    if (done)
        *done = 0;
}

int tchdone(tchan tch) {
    int rc;
    do {
        rc = close(tch->ch->fd[1]);
        if (rc == -1 && errno != EINTR)
            return -1;
    } while (rc != 0);
    tch->ch->done = 1;
    return 0;
}
