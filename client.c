/*
 * client.c
 *
 * Purpose:
 * Terminal client for the CSC209 A3 multiplayer game.
 * This file will eventually connect to the authoritative server, send lobby
 * messages such as JOIN and READY, and display server responses to the user.
 *
 * Current scope:
 * Thin terminal client. Connects to the server over TCP, multiplexes stdin and
 * the socket using select(), forwards user-typed lines to the server, and
 * prints server responses line-by-line.
 */

#define _POSIX_C_SOURCE 200112L

#include "client.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define STRINGIFY_VALUE(x) #x
#define STRINGIFY(x) STRINGIFY_VALUE(x)

enum {
    CLIENT_READ_CHUNK_SIZE = 256
};

static void client_print_usage(const char *program_name);
static int client_connect(const char *host, const char *port_text);
static int client_send_all(int fd, const char *buffer, size_t len);
static int client_run_loop(int fd);
static int client_byte_is_allowed(unsigned char byte);
static int client_handle_socket_data(const char *chunk,
                                     ssize_t chunk_len,
                                     char *line_buffer,
                                     size_t *line_len);

static void client_print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s host [port]\n", program_name);
}

static int client_connect(const char *host, const char *port_text) {
    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *current;
    int fd;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    status = getaddrinfo(host, port_text, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "client: getaddrinfo failed for %s:%s: %s\n",
                host, port_text, gai_strerror(status));
        return -1;
    }

    fd = -1;
    for (current = result; current != NULL; current = current->ai_next) {
        fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static int client_send_all(int fd, const char *buffer, size_t len) {
    size_t total_sent;

    total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(fd, buffer + total_sent, len - total_sent, 0);
        if (sent < 0) {
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}

static int client_byte_is_allowed(unsigned char byte) {
    if (byte == '\n') {
        return 1;
    }

    return byte >= 32 && byte <= 126;
}

static int client_handle_socket_data(const char *chunk,
                                     ssize_t chunk_len,
                                     char *line_buffer,
                                     size_t *line_len) {
    ssize_t i;

    for (i = 0; i < chunk_len; i++) {
        unsigned char byte = (unsigned char)chunk[i];

        if (!client_byte_is_allowed(byte)) {
            fprintf(stderr, "client: received invalid byte from server\n");
            return -1;
        }

        if (*line_len >= PROTOCOL_MAX_LINE_LEN) {
            fprintf(stderr, "client: received overlong line from server\n");
            return -1;
        }

        line_buffer[*line_len] = (char)byte;
        (*line_len)++;

        if (byte != '\n') {
            continue;
        }

        line_buffer[*line_len] = '\0';
        fputs(line_buffer, stdout);
        fflush(stdout);
        *line_len = 0;
    }

    return 0;
}

static int client_run_loop(int fd) {
    char socket_chunk[CLIENT_READ_CHUNK_SIZE];
    char socket_line_buffer[PROTOCOL_LINE_BUFFER_SIZE];
    size_t socket_line_len;
    int stdin_open;

    socket_line_len = 0;
    socket_line_buffer[0] = '\0';
    stdin_open = 1;

    for (;;) {
        fd_set read_fds;
        int max_fd;
        int ready_count;

        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        max_fd = fd;

        if (stdin_open) {
            FD_SET(STDIN_FILENO, &read_fds);
            if (STDIN_FILENO > max_fd) {
                max_fd = STDIN_FILENO;
            }
        }

        ready_count = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("client: select");
            return 1;
        }

        if (FD_ISSET(fd, &read_fds)) {
            ssize_t bytes_read = recv(fd, socket_chunk, sizeof(socket_chunk), 0);
            if (bytes_read < 0) {
                perror("client: recv");
                return 1;
            }

            if (bytes_read == 0) {
                fprintf(stderr, "client: server disconnected\n");
                return 0;
            }

            if (client_handle_socket_data(socket_chunk, bytes_read,
                                          socket_line_buffer,
                                          &socket_line_len) != 0) {
                return 1;
            }
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &read_fds)) {
            char line[PROTOCOL_LINE_BUFFER_SIZE];
            size_t len;

            if (fgets(line, sizeof(line), stdin) == NULL) {
                stdin_open = 0;
                shutdown(fd, SHUT_WR);
                continue;
            }

            len = strlen(line);
            if (len == 0) {
                continue;
            }

            if (line[len - 1] != '\n') {
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {
                }

                fprintf(stderr, "client: input line too long (max %d)\n",
                        PROTOCOL_MAX_LINE_LEN);
                continue;
            }

            if (len > (size_t)PROTOCOL_MAX_LINE_LEN) {
                fprintf(stderr, "client: input line too long (max %d)\n",
                        PROTOCOL_MAX_LINE_LEN);
                continue;
            }

            if (client_send_all(fd, line, len) != 0) {
                perror("client: send");
                return 1;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    const char *host;
    const char *port_text = STRINGIFY(PORT);
    int fd;

    if (argc < 2 || argc > 3) {
        client_print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    host = argv[1];
    if (argc == 3) {
        port_text = argv[2];
    }

    fd = client_connect(host, port_text);
    if (fd < 0) {
        fprintf(stderr, "client: could not connect to %s:%s\n", host, port_text);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "client: connected to %s:%s\n", host, port_text);
    return client_run_loop(fd);
}
