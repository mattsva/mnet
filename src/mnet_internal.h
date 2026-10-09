#ifndef MNET_INTERNAL_H
#define MNET_INTERNAL_H

/* realpath() and clock_gettime() need feature-test macros that a plain
 * -std=c17 translation unit does not get. Define them here (before any system
 * header is pulled in) unless the consumer already requested a wider level. */
#if !defined(_GNU_SOURCE) && !defined(_DEFAULT_SOURCE) && \
    !defined(_BSD_SOURCE) && !defined(_XOPEN_SOURCE)
    #define _DEFAULT_SOURCE 1
#endif
#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE + 0) < 200809L
    #undef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
#elif !defined(_POSIX_C_SOURCE)
    #define _POSIX_C_SOURCE 200809L
#endif

/*
 * Internal helpers shared by the mnet translation units.
 *
 * These are deliberately kept out of include/mnet_compat.h: they are not part
 * of the public API and several of them need POSIX feature-test macros that a
 * library consumer should not have to define. Every mnet source file starts
 * with #define _GNU_SOURCE, so the POSIX variants resolve correctly here.
 */

#include "mnet_app.h"
#include "mnet_compat.h"
#include "mnet_request.h"
#include "mnet_response.h"
#include "mnet_router.h"
#include "mnet_socket.h"

#ifdef _WIN32
    /* windows.h comes in through mnet_compat.h; the extra headers below cover
     * the inline helpers declared further down. */
    #include <time.h>
#else
    #include <stdlib.h> /* realpath() */
    #include <time.h>   /* clock_gettime(), struct timespec */
#endif

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Emit a log message through the process-wide handler if one is installed,
 * otherwise to stderr. Used for messages that are not tied to a specific app
 * (socket setup, listener failures). Never called with untrusted format
 * strings.
 */
void mnet_log_msg(int level, const char *fmt, ...);

/* Set the process-wide handler consulted by mnet_log_msg(). Defined in
 * mnet_log.c; mnet_set_log_handler() (mnet_app.c) installs it. */
void mnet_log_set_handler(mnet_log_handler_t handler);

/* Log through the per-app handler when one is set, otherwise fall back to
 * mnet_log_msg(). Defined in mnet_log.c; used by every module that serves
 * requests. */
void mnet_app_log(mnet_app_t *app, int level, const char *fmt, ...);

/* Millisecond-resolution monotonic clock for deadlines. time(NULL) has
   1-second resolution, which makes sub-second timeouts unreliable. */
#ifdef _WIN32
static inline int64_t now_ms_mono(void)
{
    return (int64_t)GetTickCount64();
}
#else
static inline int64_t now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* poll() is not available on Windows. Use select() instead. On Windows the
   first argument (nfds) is ignored — it exists only for Berkeley socket
   compatibility — so pass 0 rather than fd + 1 (fd is a SOCKET handle, and
   casting it to int would both warn and risk truncation). */
#ifdef _WIN32
static inline int mnet_poll(mnet_socket_t fd, int timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(0, &fds, NULL, NULL, &tv);
}
#else
#include <poll.h>
static inline int mnet_poll(int fd, int timeout_ms)
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms);
}
#endif

/* ---- shared limits and constants ---- */

#define MNET_INITIAL_ROUTE_CAPACITY 8
#define MNET_REQUEST_BUFFER_SIZE 8192
#define MNET_MAX_HEADER_BYTES MNET_REQUEST_BUFFER_SIZE
#define MNET_MAX_HEADERS 100
#define MNET_MAX_HEADER_LINE 4096
#define MNET_MAX_QUERY_NAME 1024
#define MNET_MAX_QUERY_VALUE 1024
#define MNET_MAX_BODY_SIZE (16 * 1024 * 1024)
#define MNET_MAX_PARAMS 16
#define MNET_MAX_QUERY 16
#define MNET_MAX_WORKERS 64
#define MNET_DEFAULT_WORKERS 4
#define MNET_DEFAULT_TIMEOUT 30
/* Idle keep-alive connections occupy a worker thread for their whole wait, so
   the default is short: a new client is never delayed more than this long by
   idle keep-alive connections. mnet_set_keep_alive_timeout() overrides it. */
#define MNET_DEFAULT_KEEP_ALIVE_TIMEOUT 5
#define MNET_LISTEN_BACKLOG 128

/* Parse/validation outcomes, surfaced to the connection handler. */
#define MNET_PARSE_OK 0
#define MNET_PARSE_BAD_REQUEST 1
#define MNET_PARSE_TOO_LARGE 2
#define MNET_PARSE_HEADERS_TOO_LARGE 3
#define MNET_PARSE_UNSUPPORTED_METHOD 4
#define MNET_PARSE_BAD_CONTENT_LENGTH 5
#define MNET_PARSE_DUPLICATE_CONTENT_LENGTH 6
#define MNET_PARSE_UNSUPPORTED_TRANSFER_ENCODING 7

/* ---- opaque application state ---- */

typedef struct {
    char *url_prefix;
    char *fs_path;
    mnet_app_t *app; /* for logging; not owned */
} static_config_t;

struct mnet_app {
    mnet_route_t *routes;
    size_t route_count;
    size_t route_capacity;
    /*
     * Set from a signal handler, so it must be a volatile sig_atomic_t: the
     * C standard only guarantees that type is safe to write from a handler and
     * read asynchronously elsewhere.
     */
    volatile sig_atomic_t running;
    int debug;
    int dev;
    int https;
    int port; /* listening port, set by mnet_run() */
    mnet_response_t (*not_found_handler)(mnet_request_t *req);
    mnet_middleware_t middleware;
    int timeout_seconds;
    int max_connections;
    int keep_alive_timeout;
    size_t max_body_size;
    mnet_log_handler_t log_handler;
    int workers;
    int active_connections; /* guarded by the pool mutex */
};

/* One parsed request, owned by the connection handler until dispatch ends. */
typedef struct {
    char method[16];
    char path[2048];
    char path_only[2048];
    char *query_string;
    char *body;
    size_t body_length;
    int body_heap;
    mnet_http_method_t method_enum;
    request_extras_t extras;
    int keep_alive;
} parsed_request_t;

/* ---- cross-module declarations ---- */

/* Response writing (mnet_dispatch.c) */
void send_response(mnet_socket_t client, const mnet_response_t *r,
    int head_only, int keep_alive);
void send_simple_error(mnet_socket_t client, int status,
    const char *status_text, const char *message, int debug);
void send_not_found(mnet_socket_t client, const char *path, int debug,
    int keep_alive);
void send_method_not_allowed(mnet_socket_t client, const char *method,
    int debug, int keep_alive);
mnet_response_t dispatch(mnet_app_t *app, parsed_request_t *parsed,
    const char **param_values, size_t max_params, size_t *out_param_count);

/* Built-in handlers (mnet_handlers.c) */
mnet_response_t welcome_response(mnet_app_t *app);
mnet_response_t static_handler(mnet_request_t *req);

/* Request parsing (mnet_parse.c) */
int parse_request(mnet_socket_t client, const char *buffer,
    ssize_t received, parsed_request_t *out, size_t max_body_size,
    int timeout_ms);
void free_extras(request_extras_t *e);

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
