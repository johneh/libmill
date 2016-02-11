#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include "libmill.h"

coroutine static void dot(char c, int *done) {
    while (! *done) {
        printf("%c", c);
        msleep(now()+5);
    }
}

static void *produce(void *p) {
    tchan xch = p;
    int i;

    mill_init();
    for (i = 1; i <= 10; i++) {
        msleep(now() + (random() % 50));
        tchs(xch, int, i, NULL);
    }
    tchdone(xch);
    mill_free();
    return NULL;
}

coroutine void xrecv(tchan xch, chan ch) {
    int done = 0;
    do {
        int k = tchr(xch, int, & done);
        if (done) {
            chdone(ch, int, 0);
            break;
        }
        chs(ch, int, k);
    } while (1);
}

int main(void) {
    pthread_t thrd1;
    tchan xch;
    int done = 0;

    mill_init();
    xch = tchmake(sizeof (int));
    go(dot('.', &done));

    chan ch = chmake(int, 1);
    go(xrecv(xch, ch));

    pthread_create(& thrd1, NULL, produce, tchdup(xch));

    while (! done) {
        choose {
        in(ch, int, val):
            printf("Got %d\n", val);
            done = ! val;
        otherwise:
            yield();
        end
        }
    }

    tchclose(xch);
    mill_free();
    return 0;
}
