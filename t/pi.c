#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "libmill.h"
double drandom(void) {
    return ((double) random()) / ((double) RAND_MAX + 1.0);
}

struct pi_s {
    int n;
    double pi;
};

void calc_pi(void *q) {
    struct pi_s *s = q;
    int hit = 0;
    int i;
    for (i = 0; i < s->n; i++) {
        double x = drandom();
        double y = drandom();
        if (sqrt(x*x+y*y) <= 1.0)
            hit++;
    }
    s->pi = 4.0 * hit / s->n;
}

coroutine void do_pi(waitgroup *wg, int n, int64_t deadline) {
    struct pi_s s;
    s.n = n;
    waitgroup_add(wg);
    if (-1 == task_run(calc_pi, &s, deadline))
        printf("pi (n = %d) Cancelled\n", n);
    else
        printf("pi (n = %d) = %g\n", n, s.pi);
}

int main(void) {
    waitgroup wg = WAITGROUP_INITIALIZER;
    mill_init();
    go(do_pi(&wg, 100000, -1));
    go(do_pi(&wg, 10000, -1));
    go(do_pi(&wg, 1000, -1));
    go(do_pi(&wg, 100,-1));
    go(do_pi(&wg, 500000, now()+5));
    go(do_pi(&wg, 1000000, now()+10));
    /* msleep(now()+1000); */
    waitgroup_wait(&wg);
    return 0;
}
