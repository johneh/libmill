/*

  Copyright (c) 2015 Martin Sustrik

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "cr.h"
#include "debug.h"
#include "libmill.h"
#include "poller.h"
#include "stack.h"
#include "utils.h"
#include "task.h"

static pthread_once_t taskq_initialized = PTHREAD_ONCE_INIT;

__thread mill_t *mill = NULL;

volatile int mill_unoptimisable1 = 1;
volatile void *mill_unoptimisable2 = NULL;

static void *mill_getvalbuf(struct mill_cr *cr) {
    if(mill_fast(cr != &mill->main)) {
            return (void*)(((char*)cr) - mill->valbuf_size);
    }
    return (void*)mill->main_valbuf;
}

/* Allocates new stack. Returns pointer to the *top* of the stack.
   For now we assume that the stack grows downwards. */
void *mill_allocstack(void) {
    if(!mill_slist_empty(&mill->cached_stacks)) {
        --mill->num_cached_stacks;
        return (void*)(mill_slist_pop(&mill->cached_stacks) + 1);
    }
    void *ptr = mill_allocstackmem();
    if(!ptr)
        mill_panic("not enough memory to allocate coroutine stack");
    return ptr;
}

int mill_suspend(void) {
    /* Even if process never gets idle, we have to process external events
       once in a while. The external signal may very well be a deadline or
       a user-issued command that cancels the CPU intensive operation. */
    struct mill_cr *mill_running = mill->running;
    if(mill->counter >= 103) {
        mill_wait(0);
        mill->counter = 0;
    }
    if(mill->counter >= 23 && mill->num_tasks >= 23) {
        mill_wait(0);
        mill->counter = 0;
    }
    /* Store the context of the current coroutine, if any. */
    if(mill_running && mill_setjmp(&mill_running->ctx))
        return mill_running->result;
    while(1) {
        /* If there's a coroutine ready to be executed go for it. */
        if(!mill_slist_empty(&mill->ready)) {
            ++mill->counter;
            struct mill_slist_item *it = mill_slist_pop(&mill->ready);
            mill->running = mill_running = mill_cont(it, struct mill_cr, ready);
            mill_jmp(&mill_running->ctx);
        }
        /* Otherwise, we are going to wait for sleeping coroutines
           and for external events. */
        mill_wait(1);
        mill_assert(!mill_slist_empty(&mill->ready));
        mill->counter = 0;
    }
}

void mill_resume(struct mill_cr *cr, int result) {
    cr->result = result;
    cr->state = MILL_READY;
    mill_slist_push_back(&mill->ready, &cr->ready);
}

/* The intial part of go(). Starts the new coroutine.
   Returns the pointer to the top of its stack. */
void *mill_go_prologue(const char *created) {
    /* Ensure that debug functions are available whenever a single go()
       statement is present in the user's code. */
    mill_preserve_debug();
    mill_assert(mill != NULL);
    /* Allocate and initialise new stack. */
    struct mill_cr *cr = ((struct mill_cr*)mill_allocstack()) - 1;
    /* mill_register_cr(cr, created); */
    mill_list_insert(&mill->all_crs, &cr->item, NULL);
    cr->cls = NULL;
    cr->wg = NULL;
#ifdef MILLDEBUG
    cr->fd = -1;
    cr->events = 0;
#endif
    mill->num_cr++;
    mill_trace(created, "{%d}=go()", (int)cr->debug.id);
    /* Suspend the parent coroutine and make the new one running. */
    if(mill_setjmp(&mill->running->ctx))
        return NULL;
    mill_resume(mill->running, 0);
    mill->running = cr;
    /* Return pointer to the top of the stack. There's valbuf interposed
       between the mill_cr structure and the stack itself. */
    return (void*)(((char*)cr) - mill->valbuf_size);
}

/* The final part of go(). Cleans up after the coroutine is finished. */
void mill_go_epilogue(void) {
    struct mill_cr *mill_running = mill->running;
    mill_trace(NULL, "go() done");
    if (mill_running->wg) {
        waitgroup *wg = mill_running->wg;
        if (--wg->counter <= 0 && wg->waiter) {
            mill_resume(wg->waiter, 0);
            wg->waiter = NULL;
        }
    }
    /* mill_unregister_cr(mill_running); */
    mill_list_erase(&mill->all_crs, &mill_running->item);

    mill_freestack(mill_running + 1);
    mill->num_cr--;
    mill->running = NULL;
    if (mill->waitmain &&
        (mill->num_cr == 0
                || (mill->num_cr == 1
                    && mill->tasks_fd[0] != -1
                    && mill->num_tasks <= 0)
        )
    ) {
        mill->waitmain = 0;
        mill_resume(&mill->main, mill->num_cr);
    }
    /* Given that there's no running coroutine at this point
       this call will never return. */
    mill_suspend();
}

void mill_yield(const char *current) {
    mill_trace(current, "yield()");
    mill_set_current(&mill->running->debug, current);
    /* This looks fishy, but yes, we can resume the coroutine even before
       suspending it. */
    mill_resume(mill->running, 0);
    mill_suspend();
}

/* XXX: max size fixed at 128 */
void *mill_valbuf(struct mill_cr *cr, size_t size) {
    mill_assert(size <= 128);
    return mill_getvalbuf(cr);
}

void *cls(void) {
    return mill->running->cls;
}

void setcls(void *val) {
    mill->running->cls = val;
}

/* TODO: stacksize argument */
void *mill_init(void) {
    (void) pthread_once(&taskq_initialized, init_workers);
    mill = malloc(sizeof (mill_t));
    if (! mill)
        return NULL;
    memset(mill, '\0', sizeof (mill_t));
    mill->tasks_fd[0] = mill->tasks_fd[1] = -1;
    struct mill_cr *mill_main = &mill->main;
    mill->valbuf_size = 128;
    mill->all_crs.first = &mill_main->item;
    mill->all_crs.last = &mill_main->item;
    mill->running = mill_main;

    mill_poller_init();
    return mill;
}

void mill_free(void) {
    if (mill) {
        mill_poller_fini();
        if (mill->tasks_fd[0] != -1) {
            (void) close(mill->tasks_fd[0]);
            (void) close(mill->tasks_fd[1]);
        }
        /* mill_purgestacks(); */
        free(mill);
        mill = NULL;
    }
}

waitgroup WAITGROUP_INITIALIZER = {0, NULL};

void waitgroup_add(waitgroup *wg) {
    /* ignore main; replace current group if any */
    if (mill->running != &mill->main) {
        if (mill->running->wg)
            mill->running->wg->counter--;
        wg->counter++;
        mill->running->wg = wg;
    }
}

int waitgroup_wait(waitgroup *wg) {
    if (wg == NULL) {
        if (mill->running != &mill->main) {
            errno = EDEADLK;
            return -1;
        }
        if (mill->num_cr > 0) {
            mill->waitmain = 1;
            mill_suspend();
        }
        return 0;
    }

    if (wg == mill->running->wg) {
        errno = EDEADLK;
        return -1;
    }
    if (wg->counter > 0) {
        wg->waiter = mill->running;
        mill_suspend();
    }
    return 0;
}
