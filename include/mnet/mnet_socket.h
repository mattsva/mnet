#ifndef MNET_SOCKET_H
#define MNET_SOCKET_H

#include "mnet_compat.h"
#include <stdint.h>
#include <sys/types.h>

#ifdef _WIN32
typedef uintptr_t mnet_socket_t;
#define MNET_INVALID_SOCKET INVALID_SOCKET
#else
typedef int mnet_socket_t;
#define MNET_INVALID_SOCKET (-1)
#endif

mnet_socket_t mnet_tcp_listen(
    uint16_t port,
    int backlog
);

mnet_socket_t mnet_tcp_accept(
    mnet_socket_t server
);

ssize_t mnet_send(
    mnet_socket_t socket,
    const void *data,
    size_t length
);

ssize_t mnet_recv(
    mnet_socket_t socket,
    void *buffer,
    size_t length
);

void mnet_close(
    mnet_socket_t socket
);

#endif
