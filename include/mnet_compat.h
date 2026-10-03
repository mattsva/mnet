#ifndef MNET_COMPAT_H
#define MNET_COMPAT_H

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>

    typedef int64_t ssize_t;

    #define strcasecmp _stricmp
    #define strdup _strdup
    #define strtok_r strtok_s

    #ifndef _DARWIN_C_SOURCE
        #define _DARWIN_C_SOURCE
    #endif

    static inline char *strcasestr(const char *haystack, const char *needle)
    {
        if (!haystack || !needle) return NULL;
        size_t needle_len = strlen(needle);
        if (needle_len == 0) return (char *)haystack;
        for (; *haystack; haystack++) {
            if (_strnicmp(haystack, needle, needle_len) == 0)
                return (char *)haystack;
        }
        return NULL;
    }
#else
    #include <strings.h>
#endif

#endif
