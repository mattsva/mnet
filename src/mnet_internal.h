#ifndef MNET_INTERNAL_H
#define MNET_INTERNAL_H

/*
 * Internal helpers shared by the mnet translation units.
 *
 * These are deliberately kept out of include/mnet_compat.h: they are not part
 * of the public API and several of them need POSIX feature-test macros that a
 * library consumer should not have to define. Every mnet source file starts
 * with #define _GNU_SOURCE, so the POSIX variants resolve correctly here.
 */

#include "mnet_compat.h"
#include "mnet_app.h"

#include <errno.h>
#include <stdlib.h>

/*
 * Emit a log message through the process-wide handler if one is installed,
 * otherwise to stderr. Used for messages that are not tied to a specific app
 * (socket setup, listener failures). Never called with untrusted format
 * strings.
 */
void mnet_log_msg(int level, const char *fmt, ...);

/*
 * Minimal threading layer.
 *
 * The worker pool needs threads, a mutex and two condition variables. POSIX
 * builds use pthreads; Windows builds use the Win32 primitives directly so
 * that MSVC (which has no pthread.h) still compiles. The two are wrapped
 * behind the same small set of calls.
 */

#ifdef _WIN32

    #include <direct.h>
    #include <malloc.h>
    #include <signal.h>
    #include <string.h>

    typedef HANDLE mnet_thread_t;
    typedef CRITICAL_SECTION mnet_mutex_t;
    typedef CONDITION_VARIABLE mnet_cond_t;

    #define MNET_THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
    #define MNET_THREAD_RETURN return 0

    static inline void mnet_mutex_init(mnet_mutex_t *m)
    {
        InitializeCriticalSection(m);
    }

    static inline void mnet_mutex_destroy(mnet_mutex_t *m)
    {
        DeleteCriticalSection(m);
    }

    static inline void mnet_mutex_lock(mnet_mutex_t *m)
    {
        EnterCriticalSection(m);
    }

    static inline void mnet_mutex_unlock(mnet_mutex_t *m)
    {
        LeaveCriticalSection(m);
    }

    static inline void mnet_cond_init(mnet_cond_t *c)
    {
        InitializeConditionVariable(c);
    }

    static inline void mnet_cond_destroy(mnet_cond_t *c)
    {
        (void)c;
    }

    static inline void mnet_cond_wait(mnet_cond_t *c, mnet_mutex_t *m)
    {
        SleepConditionVariableCS(c, m, INFINITE);
    }

    static inline void mnet_cond_signal(mnet_cond_t *c)
    {
        WakeConditionVariable(c);
    }

    static inline void mnet_cond_broadcast(mnet_cond_t *c)
    {
        WakeAllConditionVariable(c);
    }

    static inline int mnet_thread_create(mnet_thread_t *t,
        DWORD (WINAPI *fn)(LPVOID), void *arg)
    {
        *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
        return (*t == NULL) ? -1 : 0;
    }

    static inline void mnet_thread_join(mnet_thread_t t)
    {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }

    #define MNET_EINTR WSAEINTR

    /* Case-insensitive substring search (a GNU extension on Linux). */
    static inline char *strcasestr(const char *haystack, const char *needle)
    {
        size_t needle_len;

        if (haystack == NULL || needle == NULL) {
            return NULL;
        }

        needle_len = strlen(needle);
        if (needle_len == 0) {
            return (char *)haystack;
        }

        for (; *haystack != '\0'; haystack++) {
            if (_strnicmp(haystack, needle, needle_len) == 0) {
                return (char *)haystack;
            }
        }

        return NULL;
    }

    /* Canonical absolute path (caller frees); NULL on failure. */
    static inline char *mnet_realpath(const char *path)
    {
        return _fullpath(NULL, path, 0);
    }

    /* Sleep for the given number of milliseconds. */
    static inline void mnet_sleep_ms(unsigned int ms)
    {
        Sleep(ms);
    }

    /* Atomic counter helpers. */
    #define mnet_atomic_inc(p) InterlockedIncrement((volatile LONG *)(p))
    #define mnet_atomic_dec(p) InterlockedDecrement((volatile LONG *)(p))

    /* Per-socket receive/send timeout, in seconds. */
    static inline void mnet_set_socket_timeout(SOCKET s, int seconds)
    {
        DWORD ms = (DWORD)seconds * 1000u;

        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
    }

#else /* POSIX */

    #include <pthread.h>
    #include <string.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <time.h>

    typedef pthread_t mnet_thread_t;
    typedef pthread_mutex_t mnet_mutex_t;
    typedef pthread_cond_t mnet_cond_t;

    #define MNET_THREAD_FN(name) static void *name(void *arg)
    #define MNET_THREAD_RETURN return NULL

    static inline void mnet_mutex_init(mnet_mutex_t *m)
    {
        pthread_mutex_init(m, NULL);
    }

    static inline void mnet_mutex_destroy(mnet_mutex_t *m)
    {
        pthread_mutex_destroy(m);
    }

    static inline void mnet_mutex_lock(mnet_mutex_t *m)
    {
        pthread_mutex_lock(m);
    }

    static inline void mnet_mutex_unlock(mnet_mutex_t *m)
    {
        pthread_mutex_unlock(m);
    }

    static inline void mnet_cond_init(mnet_cond_t *c)
    {
        pthread_cond_init(c, NULL);
    }

    static inline void mnet_cond_destroy(mnet_cond_t *c)
    {
        pthread_cond_destroy(c);
    }

    static inline void mnet_cond_wait(mnet_cond_t *c, mnet_mutex_t *m)
    {
        pthread_cond_wait(c, m);
    }

    static inline void mnet_cond_signal(mnet_cond_t *c)
    {
        pthread_cond_signal(c);
    }

    static inline void mnet_cond_broadcast(mnet_cond_t *c)
    {
        pthread_cond_broadcast(c);
    }

    static inline int mnet_thread_create(mnet_thread_t *t,
        void *(*fn)(void *), void *arg)
    {
        return pthread_create(t, NULL, fn, arg);
    }

    static inline void mnet_thread_join(mnet_thread_t t)
    {
        pthread_join(t, NULL);
    }

    #define MNET_EINTR EINTR

    /* Canonical absolute path (caller frees); NULL on failure. */
    static inline char *mnet_realpath(const char *path)
    {
        return realpath(path, NULL);
    }

    /* Sleep for the given number of milliseconds. */
    static inline void mnet_sleep_ms(unsigned int ms)
    {
        struct timespec ts;

        ts.tv_sec = (time_t)(ms / 1000u);
        ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
        nanosleep(&ts, NULL);
    }

    /* Atomic counter helpers. */
    #define mnet_atomic_inc(p) __sync_fetch_and_add((p), 1)
    #define mnet_atomic_dec(p) __sync_fetch_and_sub((p), 1)

    /* Per-socket receive/send timeout, in seconds. */
    static inline void mnet_set_socket_timeout(int s, int seconds)
    {
        struct timeval tv;

        tv.tv_sec = seconds;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

#endif

#endif /* MNET_INTERNAL_H */
