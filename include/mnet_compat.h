#ifndef MNET_COMPAT_H
#define MNET_COMPAT_H

/*
 * Public platform compatibility layer.
 *
 * This header is pulled in by the public headers, so it must stay free of
 * function definitions: a consumer that does not define _GNU_SOURCE would
 * otherwise fail to compile. It only carries the platform headers, types and
 * macros that the public API needs. Internal helpers live in
 * src/mnet_internal.h.
 */

#ifdef _WIN32

    /* winsock2.h must precede windows.h to avoid winsock 1.1 clashes. */
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>

    #include <stddef.h>
    #include <stdint.h>
    #include <sys/types.h>

    /* MSVC does not define ssize_t; POSIX code expects it. */
    #ifndef _SSIZE_T_DEFINED
    typedef intptr_t ssize_t;
    #define _SSIZE_T_DEFINED
    #endif

    /* POSIX spellings mapped onto their Win32 equivalents. */
    #define strcasecmp _stricmp
    #define strncasecmp _strnicmp
    #define strdup _strdup
    #define strtok_r strtok_s

    /* Socket helpers: Windows uses SOCKET and a WSA error code. */
    #define mnet_socket_close(s) closesocket(s)
    #define mnet_socket_errno()  WSAGetLastError()

#else /* POSIX */

    #include <errno.h>
    #include <sys/types.h>
    #include <unistd.h>

    #define mnet_socket_close(s) close(s)
    #define mnet_socket_errno()  errno

#endif

#endif /* MNET_COMPAT_H */
