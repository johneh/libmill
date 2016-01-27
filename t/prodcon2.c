#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include "libmill.h"

/* Example: Multiple producers and a single consumer */

coroutine static void dot(char c, int k, int *done) {
    while (! *done) {
        printf("%c", c);
        msleep(now() + k * 25);
    }
}

int pid = 0;

static void *produce(void *p) {
    tchan tch = p;
    int i;
    int done = 0;
    int producer_id = __sync_add_and_fetch(&pid, 1);

    mill_init();
    go(dot('0' + producer_id, producer_id, &done));
    for (i = 1; i <= 10; i++) {
        tchs(tch, int, i + 1000 * producer_id, NULL);
        msleep(now() + (random() % 250) );
    }

    done = 1;
    msleep(now()+100);
    mill_free();
    return NULL;
}

static void consume(tchan tch) {
    int i;
    int done = 0;
    msleep(now() + 200);
    for (i = 0; i < 30; i++) {
        int val = tchr(tch, int, & done);
        if (done)
            break;
        printf("\n\t worker %d => %d\t", val / 1000, val % 1000);
        fflush(stdout);
        msleep(now() + (random() % 200));
    }
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
        pthread_create(& thrd[i], NULL, produce, ch[i]);
    }

    consume(tch);
    for (i = 0; i < 3; i++) {
        pthread_join(thrd[i], NULL);
        tchclose(ch[i]);
    }

    done = 1;    /* signal the goroutine to quit */
    msleep(now()+50);
    tchclose(tch);
    printf("\n");
    mill_free();
    return 0;
}
