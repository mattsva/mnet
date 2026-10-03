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

#include <errno.h>
#include <stdlib.h>

#ifdef _WIN32

    #include <direct.h>
    #include <malloc.h>
    #include <signal.h>
    #include <string.h>

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

    #include <string.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <time.h>

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
