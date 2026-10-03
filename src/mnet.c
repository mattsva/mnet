#define _GNU_SOURCE
#include "mnet_socket.h"
#include "mnet_compat.h"
#include "mnet_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/*
 * Windows requires a process-wide Winsock initialisation before any socket
 * call. It is done lazily on the first listen and never torn down, which is
 * fine for a server that lives for the whole process lifetime.
 */
#ifdef _WIN32
static int mnet_wsa_init(void)
{
    static int started = 0;

    if (!started) {
        WSADATA data;

        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return -1;
        }

        started = 1;
    }

    return 0;
}
#endif

mnet_socket_t mnet_tcp_listen(
    uint16_t port,
    int backlog)
{
    if (backlog <= 0) {
        mnet_log_msg(MNET_LOG_ERROR, "invalid listen backlog %d", backlog);
        errno = EINVAL;
        return MNET_INVALID_SOCKET;
    }

#ifdef _WIN32
    if (mnet_wsa_init() != 0) {
        return MNET_INVALID_SOCKET;
    }
#endif

    char port_string[6];

    int written = snprintf(
        port_string,
        sizeof(port_string),
        "%u",
        (unsigned int)port
    );

    if (written < 0 ||
        (size_t)written >= sizeof(port_string)) {
        errno = EINVAL;
        return MNET_INVALID_SOCKET;
    }

    struct addrinfo hints = {0};
    struct addrinfo *results = NULL;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int error = getaddrinfo(
        NULL,
        port_string,
        &hints,
        &results
    );

    if (error != 0) {
        mnet_log_msg(MNET_LOG_ERROR,
            "getaddrinfo failed for port %u: %d", (unsigned int)port, error);
        errno = EINVAL;
        return MNET_INVALID_SOCKET;
    }

    mnet_socket_t server = MNET_INVALID_SOCKET;

    for (struct addrinfo *entry = results;
         entry != NULL;
         entry = entry->ai_next) {

        mnet_socket_t fd = socket(
            entry->ai_family,
            entry->ai_socktype,
            entry->ai_protocol
        );

        if (fd == MNET_INVALID_SOCKET) {
            continue;
        }

        int reuse = 1;

#ifdef _WIN32
        if (setsockopt(
                fd,
                SOL_SOCKET,
                SO_REUSEADDR,
                (const char *)&reuse,
                sizeof(reuse)) != 0) {
            mnet_socket_close(fd);
            continue;
        }
#else
        if (setsockopt(
                fd,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse,
                sizeof(reuse)) != 0) {
            mnet_socket_close(fd);
            continue;
        }
#endif

        if (bind(
                fd,
                entry->ai_addr,
                (int)entry->ai_addrlen) != 0) {
            mnet_socket_close(fd);
            continue;
        }

        if (listen(fd, backlog) != 0) {
            mnet_socket_close(fd);
            continue;
        }

        server = fd;
        break;
    }

    freeaddrinfo(results);

    return server;
}

mnet_socket_t mnet_tcp_accept(
    mnet_socket_t server)
{
    return accept(
        server,
        NULL,
        NULL
    );
}

ssize_t mnet_send(
    mnet_socket_t socket,
    const void *data,
    size_t length)
{
    if (data == NULL && length != 0) {
        errno = EINVAL;
        return -1;
    }

    return (ssize_t)send(
        socket,
        (const char *)data,
        (int)length,
        0
    );
}

ssize_t mnet_recv(
    mnet_socket_t socket,
    void *buffer,
    size_t length)
{
    if (buffer == NULL && length != 0) {
        errno = EINVAL;
        return -1;
    }

    return (ssize_t)recv(
        socket,
        (char *)buffer,
        (int)length,
        0
    );
}

void mnet_close(
    mnet_socket_t socket)
{
    if (socket != MNET_INVALID_SOCKET) {
        mnet_socket_close(socket);
    }
}
