/* no_rt_threads.c
 * LD_PRELOAD shim that strips realtime scheduling attributes from
 * pthread_create calls. Required for Melodyne under Wine on Linux
 * where SCHED_FIFO/SCHED_RR requires CAP_SYS_NICE which regular
 * users don't have — causing pthread_create to fail with EINVAL,
 * which propagates as std::system_error and crashes the process.
 *
 * Build:
 *   gcc -shared -fPIC -o no_rt_threads.so no_rt_threads.c -lpthread -ldl
 *
 * Use:
 *   LD_PRELOAD=/path/to/no_rt_threads.so wine ara-plugin-test.exe.so ...
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <dlfcn.h>
#include <sched.h>
#include <string.h>

typedef int (*real_pthread_create_t)(pthread_t*, const pthread_attr_t*,
                                      void*(*)(void*), void*);

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start)(void*), void* arg)
{
    static real_pthread_create_t real_fn = NULL;
    if (!real_fn)
        real_fn = (real_pthread_create_t)dlsym(RTLD_NEXT, "pthread_create");

    if (attr) {
        int policy = 0;
        pthread_attr_getschedpolicy(attr, &policy);
        if (policy == SCHED_FIFO || policy == SCHED_RR) {
            /* Strip realtime scheduling — use default SCHED_OTHER */
            pthread_attr_t safe_attr;
            pthread_attr_init(&safe_attr);
            /* Copy stack size if set */
            size_t stacksize = 0;
            if (pthread_attr_getstacksize(attr, &stacksize) == 0 && stacksize > 0)
                pthread_attr_setstacksize(&safe_attr, stacksize);
            int r = real_fn(thread, &safe_attr, start, arg);
            pthread_attr_destroy(&safe_attr);
            return r;
        }
    }

    return real_fn(thread, attr, start, arg);
}
