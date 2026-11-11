#include "worker.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "log.h"
#include "loop.h"

struct job {
    struct job *next;
    lm_work_fn fn, done;
    void *data;
    const void *owner;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct job *g_head, *g_tail;                    /* waiting for the thread */
static struct job *g_done_head, *g_done_tail;          /* finished, waiting for the loop */
static pthread_t g_thread;
static bool g_up, g_stop;
static int g_efd = -1;
static struct lm_source *g_src;
static int g_pending;

static void *thread_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_head && !g_stop)
            pthread_cond_wait(&g_cond, &g_lock);
        if (!g_head)
            break;
        struct job *j = g_head;
        g_head = j->next;
        if (!g_head)
            g_tail = NULL;
        pthread_mutex_unlock(&g_lock);
        if (j->fn)
            j->fn(j->data);
        pthread_mutex_lock(&g_lock);
        j->next = NULL;
        if (g_done_tail)
            g_done_tail->next = j;
        else
            g_done_head = j;
        g_done_tail = j;
        uint64_t one = 1;
        if (write(g_efd, &one, sizeof(one)) < 0) {
            /* (a full counter: the loop is behind anyway) */
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void on_done(int fd, uint32_t events, void *data)
{
    (void)events; (void)data;
    uint64_t n;
    if (read(fd, &n, sizeof(n)) < 0) {
        /* nothing to read */
    }
    for (;;) {
        pthread_mutex_lock(&g_lock);
        struct job *j = g_done_head;
        if (j) {
            g_done_head = j->next;
            if (!g_done_head)
                g_done_tail = NULL;
            g_pending--;
        }
        pthread_mutex_unlock(&g_lock);
        if (!j)
            break;
        if (j->done)
            j->done(j->data);
        free(j);
    }
}

void lm_work_init(struct lm_loop *loop)
{
    if (g_up || !loop)
        return;
    g_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_efd < 0)
        return;
    g_src = lm_loop_add_fd(loop, g_efd, 0x001 /* EPOLLIN */, on_done, NULL);
    g_stop = false;
    sigset_t all, old;                                  /* the thread must not take the process's signals (SIGUSR2 ... go to the loop's signalfd) */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int rc = g_src ? pthread_create(&g_thread, NULL, thread_main, NULL) : -1;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        if (g_src)
            lm_source_remove(g_src);
        g_src = NULL;
        close(g_efd);
        g_efd = -1;
        return;
    }
    g_up = true;
}

void lm_work_submit(lm_work_fn fn, lm_work_fn done, void *data, const void *owner)
{
    if (!g_up) {                                        /* no thread (a client, a test): do it here */
        if (fn)
            fn(data);
        if (done)
            done(data);
        return;
    }
    struct job *j = calloc(1, sizeof(*j));
    if (!j) {
        if (fn)
            fn(data);
        if (done)
            done(data);
        return;
    }
    j->fn = fn;
    j->done = done;
    j->data = data;
    j->owner = owner;
    pthread_mutex_lock(&g_lock);
    if (g_tail)
        g_tail->next = j;
    else
        g_head = j;
    g_tail = j;
    g_pending++;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void lm_work_forget(const void *owner)
{
    if (!owner)
        return;
    pthread_mutex_lock(&g_lock);
    for (struct job *j = g_head; j; j = j->next)
        if (j->owner == owner)
            j->done = NULL;
    for (struct job *j = g_done_head; j; j = j->next)
        if (j->owner == owner)
            j->done = NULL;
    pthread_mutex_unlock(&g_lock);
}

int lm_work_pending(void)
{
    pthread_mutex_lock(&g_lock);
    int n = g_pending;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void lm_work_shutdown(void)
{
    if (!g_up)
        return;
    pthread_mutex_lock(&g_lock);
    g_stop = true;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
    g_up = false;
    if (g_src)
        lm_source_remove(g_src);
    g_src = NULL;
    if (g_efd >= 0)
        close(g_efd);
    g_efd = -1;
    while (g_done_head) {                               /* finished but never delivered: dropped */
        struct job *j = g_done_head;
        g_done_head = j->next;
        free(j);
    }
    g_done_tail = NULL;
    g_pending = 0;
}
