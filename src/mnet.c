#include "mnet_socket.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

mnet_socket_t mnet_tcp_listen(
    uint16_t port,
    int backlog)
{
    if (backlog <= 0) {
        errno = EINVAL;
        return MNET_INVALID_SOCKET;
    }

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
        errno = EINVAL;
        return MNET_INVALID_SOCKET;
    }

    mnet_socket_t server = MNET_INVALID_SOCKET;

    for (struct addrinfo *entry = results;
         entry != NULL;
         entry = entry->ai_next) {

        int fd = socket(
            entry->ai_family,
            entry->ai_socktype,
            entry->ai_protocol
        );

        if (fd == -1) {
            continue;
        }

        int reuse = 1;

        if (setsockopt(
                fd,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse,
                sizeof(reuse)) == -1) {
            close(fd);
            continue;
        }

        if (bind(
                fd,
                entry->ai_addr,
                entry->ai_addrlen) == -1) {
            close(fd);
            continue;
        }

        if (listen(fd, backlog) == -1) {
            close(fd);
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

    return send(
        socket,
        data,
        length,
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

    return recv(
        socket,
        buffer,
        length,
        0
    );
}

void mnet_close(
    mnet_socket_t socket)
{
    if (socket != MNET_INVALID_SOCKET) {
        close(socket);
    }
}
