#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include "libmill.h"

/* Example: Single producer and multiple consumers */

coroutine static void dot(char c, int k, int *done) {
    while (! *done) {
        printf("%c", c);
        msleep(now() + k * 25);
    }
}

int cid = 0;

static void *consume(void *p) {
    tchan tch = p;
    int done = 0;
    int consumer_id = __sync_add_and_fetch(&cid, 1);

    mill_init();
    go(dot('0' + consumer_id, consumer_id, &done));
    while (1) {
        int val = tchr(tch, int, &done);
        if (done)
            break;
        msleep(now() + (random() % 250) );
        printf("\n\t worker %d => %d\t", consumer_id, val);
        fflush(stdout);
    }
    waitgroup_wait(NULL);
    mill_free();
    return NULL;
}

static void produce(tchan tch) {
    int i;
    msleep(now() + 200);
    for (i = 1000; i < 1050; i++) {
        tchs(tch, int, i, NULL);
        msleep(now() + (random() % 500));
    }
    tchdone(tch);
}

int main(void) {
    pthread_t thrd[3];
    tchan tch, ch[3];
    int i;
    int done = 0;

    mill_init();

    tch = tchmake(sizeof (int));
    go(dot('.', 2, &done));

    for (i = 0; i < 3; i++) {
        ch[i] = tchdup(tch);
        pthread_create(& thrd[i], NULL, consume, ch[i]);
    }
    produce(tch);
    for (i = 0; i < 3; i++) {
        pthread_join(thrd[i], NULL);
        tchclose(ch[i]);
    }

    done = 1;    /* signal the goroutine to quit */
    waitgroup_wait(NULL);
    tchclose(tch);
    printf("\n");
    mill_free();
    return 0;
}
