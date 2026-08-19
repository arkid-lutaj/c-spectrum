/*
 * cs_thread.h - tiny threading wrapper.
 *
 * C11 <threads.h> still isn't there on MinGW-w64 and older glibc, so the two
 * things this project needs are wrapped over Win32 and pthreads by hand.
 */

#ifndef CS_THREAD_H
#define CS_THREAD_H

#include <stdbool.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef HANDLE cs_thread_t;
#else
#  include <pthread.h>
#  include <unistd.h>
typedef pthread_t cs_thread_t;
#endif

typedef void (*cs_thread_fn)(void *arg);

bool cs_thread_start(cs_thread_t *t, cs_thread_fn fn, void *arg);
void cs_thread_join(cs_thread_t *t);        /* safe on a zeroed handle */
void cs_sleep_ms(int ms);
void cs_thread_lower_priority(cs_thread_t *t);

#endif /* CS_THREAD_H */
