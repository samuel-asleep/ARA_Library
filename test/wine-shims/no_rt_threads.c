/* no_rt_threads.c
 * LD_PRELOAD shim that prevents realtime thread scheduling under Wine.
 *
 * Melodyne creates threads with SCHED_FIFO/SCHED_RR which requires
 * CAP_SYS_NICE. Without it, the calls fail with EINVAL, which propagates
 * as std::system_error and aborts the process.
 *
 * Under Wine 11, Windows PE threads are created via Wine's internal path
 * so intercepting pthread_create alone is not enough. We intercept all
 * scheduling-related entry points:
 *   - pthread_create           (attr-based policy at thread creation)
 *   - pthread_attr_setschedpolicy  (attr setup before pthread_create)
 *   - pthread_setschedparam    (priority change on running thread)
 *   - sched_setscheduler       (direct scheduler change)
 *
 * Build:
 *   gcc -shared -fPIC -o no_rt_threads.so no_rt_threads.c -lpthread -ldl
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>

/* ── pthread_create ─────────────────────────────────────────────────────── */

typedef int (*real_pthread_create_t)(pthread_t*, const pthread_attr_t*,
                                     void*(*)(void*), void*);

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start)(void*), void* arg)
{
    static real_pthread_create_t real_fn;
    if (!real_fn)
        real_fn = (real_pthread_create_t)dlsym(RTLD_NEXT, "pthread_create");

    if (attr) {
        int policy = SCHED_OTHER;
        pthread_attr_getschedpolicy(attr, &policy);
        if (policy == SCHED_FIFO || policy == SCHED_RR) {
            fprintf(stderr, "[no_rt_threads] pthread_create intercepted policy=%d — stripping\n", policy);
            fflush(stderr);
            pthread_attr_t safe;
            pthread_attr_init(&safe);
            size_t stacksize = 0;
            if (pthread_attr_getstacksize(attr, &stacksize) == 0 && stacksize > 0)
                pthread_attr_setstacksize(&safe, stacksize);
            int r = real_fn(thread, &safe, start, arg);
            pthread_attr_destroy(&safe);
            return r;
        }
    }
    return real_fn(thread, attr, start, arg);
}

/* ── pthread_attr_setschedpolicy ────────────────────────────────────────── */

typedef int (*real_attr_setpolicy_t)(pthread_attr_t*, int);

int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy)
{
    static real_attr_setpolicy_t real_fn;
    if (!real_fn)
        real_fn = (real_attr_setpolicy_t)dlsym(RTLD_NEXT, "pthread_attr_setschedpolicy");

    if (policy == SCHED_FIFO || policy == SCHED_RR)
        policy = SCHED_OTHER;
    return real_fn(attr, policy);
}

/* ── pthread_setschedparam ──────────────────────────────────────────────── */

typedef int (*real_setschedparam_t)(pthread_t, int, const struct sched_param*);

int pthread_setschedparam(pthread_t thread, int policy,
                          const struct sched_param* param)
{
    static real_setschedparam_t real_fn;
    if (!real_fn)
        real_fn = (real_setschedparam_t)dlsym(RTLD_NEXT, "pthread_setschedparam");

    if (policy == SCHED_FIFO || policy == SCHED_RR) {
        fprintf(stderr, "[no_rt_threads] pthread_setschedparam intercepted policy=%d — suppressed\n", policy);
        fflush(stderr);
        return 0;
    }
    return real_fn(thread, policy, param);
}

/* ── sched_setscheduler ─────────────────────────────────────────────────── */

typedef int (*real_sched_setscheduler_t)(pid_t, int, const struct sched_param*);

int sched_setscheduler(pid_t pid, int policy, const struct sched_param* param)
{
    static real_sched_setscheduler_t real_fn;
    if (!real_fn)
        real_fn = (real_sched_setscheduler_t)dlsym(RTLD_NEXT, "sched_setscheduler");

    if (policy == SCHED_FIFO || policy == SCHED_RR)
        return 0;
    return real_fn(pid, policy, param);
}
