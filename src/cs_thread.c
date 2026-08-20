/*
 * C-Spectrum , cs_thread.c
 */

#include "cs_thread.h"
#include <stdlib.h>
#include <string.h>

/* Win32 and pthreads want different entry point signatures, so the real
 * function and its argument get passed through this little struct. */
typedef struct {
    cs_thread_fn fn;
    void        *arg;
} Trampoline;

#ifdef _WIN32
static DWORD WINAPI win_entry(LPVOID p)
{
    Trampoline t = *(Trampoline *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}
#else
static void *posix_entry(void *p)
{
    Trampoline t = *(Trampoline *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}
#endif

bool cs_thread_start(cs_thread_t *t, cs_thread_fn fn, void *arg)
{
    Trampoline *tr = (Trampoline *)malloc(sizeof(*tr));
    if (!tr) return false;
    tr->fn  = fn;
    tr->arg = arg;

#ifdef _WIN32
    *t = CreateThread(NULL, 0, win_entry, tr, 0, NULL);
    if (*t == NULL) { free(tr); return false; }
    return true;
#else
    if (pthread_create(t, NULL, posix_entry, tr) != 0) { free(tr); return false; }
    return true;
#endif
}

void cs_thread_join(cs_thread_t *t)
{
#ifdef _WIN32
    if (t && *t) {
        WaitForSingleObject(*t, INFINITE);
        CloseHandle(*t);
        *t = NULL;
    }
#else
    if (t && *t) {
        pthread_join(*t, NULL);
        memset(t, 0, sizeof(*t));
    }
#endif
}

void cs_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

void cs_thread_lower_priority(cs_thread_t *t)
{
#ifdef _WIN32
    if (t && *t) SetThreadPriority(*t, THREAD_PRIORITY_BELOW_NORMAL);
#else
    (void)t;  /* no portable pthreads equivalent, skip it */
#endif
}
