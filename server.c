/*
 * server.c
 *
 * Purpose:
 * Main authoritative server for the CSC209 A3 multiplayer terminal game.
 * This file owns the TCP listening socket, accepts new client connections,
 * multiplexes sockets with select(), buffers client input a line at a time,
 * and removes disconnected clients cleanly.
 *
 * Current scope:
 * Networking foundation only. Lobby message semantics are limited to
 * recognizing JOIN and READY lines and routing them to TODO hooks.
 */

#define _POSIX_C_SOURCE 200112L

#include "server.h"

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
    SERVER_LISTEN_BACKLOG = PROTOCOL_MAX_PLAYERS,
    SERVER_READ_CHUNK_SIZE = 256
};

static int server_setup_listener(server_state_t *server, const char *port_text);
static int server_create_listener(const char *port_text);
static int server_run_select_loop(server_state_t *server);
static void server_shutdown(server_state_t *server);
static void server_accept_client(server_state_t *server);
static ssize_t server_find_free_slot(const server_state_t *server);
static void server_remove_client(server_state_t *server,
                                 size_t client_index,
                                 const char *reason);
static void server_handle_client_readable(server_state_t *server,
                                          size_t client_index);
static bool server_byte_is_allowed(unsigned char byte);
static int server_handle_client_line(server_state_t *server,
                                     size_t client_index,
                                     const char *line);
static int server_handle_join_line(server_state_t *server,
                                   size_t client_index,
                                   const char *line);
static int server_handle_ready_line(server_state_t *server,
                                    size_t client_index,
                                    const char *line);
static int server_send_to_client(const server_client_t *client, const char *message);
static int server_send_error_and_close(server_state_t *server,
                                       size_t client_index,
                                       const char *reason);
static int server_broadcast_message(const server_state_t *server,
                                    const char *message);
static int server_broadcast_info(const server_state_t *server, const char *text);
static bool server_username_is_unique(const server_state_t *server,
                                      const char *username);
static int server_count_joined_clients(const server_state_t *server);
static int server_count_ready_clients(const server_state_t *server);
static void server_broadcast_lobby_status(const server_state_t *server);
static void server_try_start_game(server_state_t *server);
static int server_collect_joined_players(const server_state_t *server,
                                         int *player_ids,
                                         char usernames[][PROTOCOL_MAX_USERNAME_LEN + 1]);
static void server_print_usage(const char *program_name);

void server_state_init(server_state_t *server) {
    size_t i;

    if (server == NULL) {
        return;
    }

    server->listen_fd = -1;
    server->next_player_id = 1;
    server->active_clients = 0;
    server->phase = SERVER_PHASE_LOBBY;
    game_state_init(&server->game);

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        server->clients[i].fd = -1;
        server->clients[i].active = false;
        server->clients[i].joined = false;
        server->clients[i].ready = false;
        server->clients[i].player_id = 0;
        server->clients[i].username[0] = '\0';
        server->clients[i].input_buffer[0] = '\0';
        server->clients[i].input_len = 0;
    }
}

int server_run(const char *port_text) {
    server_state_t server;
    int result;

    server_state_init(&server);

    if (server_setup_listener(&server, port_text) != 0) {
        return EXIT_FAILURE;
    }

    result = server_run_select_loop(&server);
    server_shutdown(&server);
    return result;
}

static int server_setup_listener(server_state_t *server, const char *port_text) {
    int listen_fd;

    listen_fd = server_create_listener(port_text);
    if (listen_fd < 0) {
        return -1;
    }

    server->listen_fd = listen_fd;
    fprintf(stderr, "server: listening on port %s\n", port_text);
    return 0;
}

static int server_create_listener(const char *port_text) {
    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *current;
    int listen_fd;
    int opt_value;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    status = getaddrinfo(NULL, port_text, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "server: getaddrinfo failed for port %s: %s\n",
                port_text, gai_strerror(status));
        return -1;
    }

    listen_fd = -1;
    for (current = result; current != NULL; current = current->ai_next) {
        listen_fd = socket(current->ai_family,
                           current->ai_socktype,
                           current->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }

        opt_value = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                       &opt_value, sizeof(opt_value)) < 0) {
            close(listen_fd);
            listen_fd = -1;
            continue;
        }

        if (bind(listen_fd, current->ai_addr, current->ai_addrlen) == 0) {
            break;
        }

        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(result);

    if (listen_fd < 0) {
        fprintf(stderr, "server: could not bind to port %s\n", port_text);
        return -1;
    }

    if (listen(listen_fd, SERVER_LISTEN_BACKLOG) < 0) {
        perror("server: listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

static int server_run_select_loop(server_state_t *server) {
    for (;;) {
        fd_set read_fds;
        int max_fd;
        int ready_count;
        size_t i;

        FD_ZERO(&read_fds);
        FD_SET(server->listen_fd, &read_fds);
        max_fd = server->listen_fd;

        for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
            if (!server->clients[i].active) {
                continue;
            }

            FD_SET(server->clients[i].fd, &read_fds);
            if (server->clients[i].fd > max_fd) {
                max_fd = server->clients[i].fd;
            }
        }

        ready_count = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("server: select");
            return EXIT_FAILURE;
        }

        if (FD_ISSET(server->listen_fd, &read_fds)) {
            server_accept_client(server);
            ready_count--;
        }

        for (i = 0; i < PROTOCOL_MAX_PLAYERS && ready_count > 0; i++) {
            if (!server->clients[i].active) {
                continue;
            }

            if (!FD_ISSET(server->clients[i].fd, &read_fds)) {
                continue;
            }

            server_handle_client_readable(server, i);
            ready_count--;
        }
    }
}

static void server_shutdown(server_state_t *server) {
    size_t i;

    if (server == NULL) {
        return;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (server->clients[i].active) {
            server_remove_client(server, i, "server shutdown");
        }
    }
}

static void server_accept_client(server_state_t *server) {
    ssize_t free_slot;
    int client_fd;
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    client_fd = accept(server->listen_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("server: accept");
        return;
    }

    if (server->phase != SERVER_PHASE_LOBBY) {
        protocol_format_error(message, sizeof(message), "game already started");
        send(client_fd, message, strlen(message), 0);
        close(client_fd);
        fprintf(stderr, "server: rejected connection because game already started\n");
        return;
    }

    free_slot = server_find_free_slot(server);
    if (free_slot < 0) {
        protocol_format_error(message, sizeof(message), "lobby is full");
        send(client_fd, message, strlen(message), 0);
        close(client_fd);
        fprintf(stderr, "server: rejecting connection because lobby is full\n");
        return;
    }

    server->clients[free_slot].fd = client_fd;
    server->clients[free_slot].active = true;
    server->clients[free_slot].joined = false;
    server->clients[free_slot].ready = false;
    server->clients[free_slot].player_id = 0;
    server->clients[free_slot].username[0] = '\0';
    server->clients[free_slot].input_buffer[0] = '\0';
    server->clients[free_slot].input_len = 0;
    server->active_clients++;

    fprintf(stderr, "server: accepted client into slot %zd (fd=%d)\n",
            free_slot, client_fd);
}

static ssize_t server_find_free_slot(const server_state_t *server) {
    size_t i;

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!server->clients[i].active) {
            return (ssize_t)i;
        }
    }

    return -1;
}

static void server_remove_client(server_state_t *server,
                                 size_t client_index,
                                 const char *reason) {
    int fd;

    fd = server->clients[client_index].fd;
    if (fd >= 0) {
        close(fd);
    }

    fprintf(stderr, "server: removed client in slot %zu", client_index);
    if (reason != NULL && *reason != '\0') {
        fprintf(stderr, " (%s)", reason);
    }
    fprintf(stderr, "\n");

    server->clients[client_index].fd = -1;
    server->clients[client_index].active = false;
    server->clients[client_index].joined = false;
    server->clients[client_index].ready = false;
    server->clients[client_index].player_id = 0;
    server->clients[client_index].username[0] = '\0';
    server->clients[client_index].input_buffer[0] = '\0';
    server->clients[client_index].input_len = 0;

    if (server->active_clients > 0) {
        server->active_clients--;
    }
}

static void server_handle_client_readable(server_state_t *server,
                                          size_t client_index) {
    server_client_t *client;
    char read_buffer[SERVER_READ_CHUNK_SIZE];
    ssize_t bytes_read;
    ssize_t i;

    client = &server->clients[client_index];
    bytes_read = recv(client->fd, read_buffer, sizeof(read_buffer), 0);
    if (bytes_read < 0) {
        perror("server: recv");
        server_remove_client(server, client_index, "recv failure");
        return;
    }

    if (bytes_read == 0) {
        server_remove_client(server, client_index, "peer disconnected");
        return;
    }

    for (i = 0; i < bytes_read; i++) {
        unsigned char byte = (unsigned char)read_buffer[i];
        char line[PROTOCOL_LINE_BUFFER_SIZE];

        if (!server_byte_is_allowed(byte)) {
            server_remove_client(server, client_index, "non-ASCII or invalid byte");
            return;
        }

        if (client->input_len >= PROTOCOL_MAX_LINE_LEN) {
            server_remove_client(server, client_index, "protocol line too long");
            return;
        }

        client->input_buffer[client->input_len] = (char)byte;
        client->input_len++;

        if (byte != '\n') {
            continue;
        }

        client->input_buffer[client->input_len] = '\0';
        memcpy(line, client->input_buffer, client->input_len + 1);
        client->input_buffer[0] = '\0';
        client->input_len = 0;

        if (server_handle_client_line(server, client_index, line) != 0) {
            return;
        }
    }
}

static bool server_byte_is_allowed(unsigned char byte) {
    if (byte == '\n') {
        return true;
    }

    return byte >= 32 && byte <= 126;
}

static int server_handle_client_line(server_state_t *server,
                                     size_t client_index,
                                     const char *line) {
    if (strncmp(line, PROTOCOL_MSG_JOIN "|", strlen(PROTOCOL_MSG_JOIN) + 1) == 0) {
        return server_handle_join_line(server, client_index, line);
    }

    if (strcmp(line, PROTOCOL_MSG_READY "\n") == 0) {
        return server_handle_ready_line(server, client_index, line);
    }

    server_send_to_client(&server->clients[client_index],
                          PROTOCOL_MSG_ERROR "|unsupported message\n");
    fprintf(stderr, "server: unsupported lobby message from slot %zu: %s",
            client_index, line);
    return 0;
}

static int server_handle_join_line(server_state_t *server,
                                   size_t client_index,
                                   const char *line) {
    server_client_t *client;
    char username[PROTOCOL_MAX_USERNAME_LEN + 1];
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    client = &server->clients[client_index];

    if (server->phase != SERVER_PHASE_LOBBY) {
        return server_send_error_and_close(server, client_index,
                                           "game already started");
    }

    if (client->joined) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|already joined\n");
        return 0;
    }

    if (!protocol_parse_join_username(line, username, sizeof(username))) {
        return server_send_error_and_close(server, client_index,
                                           "invalid JOIN format");
    }

    if (!protocol_username_is_valid(username)) {
        return server_send_error_and_close(server, client_index,
                                           "invalid username");
    }

    if (!server_username_is_unique(server, username)) {
        return server_send_error_and_close(server, client_index,
                                           "username already in use");
    }

    client->joined = true;
    client->ready = false;
    client->player_id = server->next_player_id;
    server->next_player_id++;
    strcpy(client->username, username);

    protocol_format_welcome(message, sizeof(message), client->player_id);
    server_send_to_client(client, message);

    snprintf(message, sizeof(message),
             "%s joined the lobby (%d/%d players)",
             client->username,
             server_count_joined_clients(server),
             PROTOCOL_MAX_PLAYERS);
    server_broadcast_info(server, message);

    fprintf(stderr, "server: slot %zu joined as %s (player_id=%d)\n",
            client_index, client->username, client->player_id);
    server_broadcast_lobby_status(server);
    return 0;
}

static int server_handle_ready_line(server_state_t *server,
                                    size_t client_index,
                                    const char *line) {
    server_client_t *client;
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    (void)line;

    client = &server->clients[client_index];

    if (!client->joined) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    if (server->phase != SERVER_PHASE_LOBBY) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|game already started\n");
        return 0;
    }

    if (client->ready) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|already ready\n");
        return 0;
    }

    client->ready = true;
    snprintf(message, sizeof(message), "%s is ready", client->username);
    server_broadcast_info(server, message);
    server_broadcast_lobby_status(server);
    server_try_start_game(server);

    fprintf(stderr, "server: slot %zu marked ready (%s)\n",
            client_index, client->username);
    return 0;
}

static int server_send_to_client(const server_client_t *client, const char *message) {
    size_t total_sent;
    size_t message_len;

    if (client == NULL || message == NULL || client->fd < 0) {
        return -1;
    }

    message_len = strlen(message);
    total_sent = 0;
    while (total_sent < message_len) {
        ssize_t sent = send(client->fd,
                            message + total_sent,
                            message_len - total_sent,
                            0);
        if (sent < 0) {
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}

static int server_send_error_and_close(server_state_t *server,
                                       size_t client_index,
                                       const char *reason) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    if (protocol_format_error(message, sizeof(message), reason) >= 0) {
        server_send_to_client(&server->clients[client_index], message);
    }

    server_remove_client(server, client_index, reason);
    return -1;
}

static int server_broadcast_message(const server_state_t *server,
                                    const char *message) {
    size_t i;

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!server->clients[i].active || !server->clients[i].joined) {
            continue;
        }

        if (server_send_to_client(&server->clients[i], message) != 0) {
            fprintf(stderr, "server: failed sending broadcast to slot %zu\n", i);
        }
    }

    return 0;
}

static int server_broadcast_info(const server_state_t *server, const char *text) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    if (protocol_format_info(message, sizeof(message), text) < 0) {
        return -1;
    }

    return server_broadcast_message(server, message);
}

static bool server_username_is_unique(const server_state_t *server,
                                      const char *username) {
    size_t i;

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!server->clients[i].active || !server->clients[i].joined) {
            continue;
        }

        if (strcmp(server->clients[i].username, username) == 0) {
            return false;
        }
    }

    return true;
}

static int server_count_joined_clients(const server_state_t *server) {
    int joined_clients;
    size_t i;

    joined_clients = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (server->clients[i].active && server->clients[i].joined) {
            joined_clients++;
        }
    }

    return joined_clients;
}

static int server_count_ready_clients(const server_state_t *server) {
    int ready_clients;
    size_t i;

    ready_clients = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (server->clients[i].active &&
            server->clients[i].joined &&
            server->clients[i].ready) {
            ready_clients++;
        }
    }

    return ready_clients;
}

static void server_broadcast_lobby_status(const server_state_t *server) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    snprintf(message, sizeof(message),
             "lobby status: %d joined, %d ready, need at least %d ready to start",
             server_count_joined_clients(server),
             server_count_ready_clients(server),
             PROTOCOL_MIN_PLAYERS);
    server_broadcast_info(server, message);
}

static void server_try_start_game(server_state_t *server) {
    int joined_clients;
    int ready_clients;
    int player_ids[PROTOCOL_MAX_PLAYERS];
    char usernames[PROTOCOL_MAX_PLAYERS][PROTOCOL_MAX_USERNAME_LEN + 1];
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    joined_clients = server_count_joined_clients(server);
    ready_clients = server_count_ready_clients(server);

    if (joined_clients < PROTOCOL_MIN_PLAYERS || ready_clients != joined_clients) {
        return;
    }

    if (server_collect_joined_players(server, player_ids, usernames) != joined_clients) {
        fprintf(stderr, "server: failed to snapshot joined players for game start\n");
        return;
    }

    if (!game_start(&server->game, player_ids, usernames, joined_clients)) {
        fprintf(stderr, "server: game_start rejected %d players\n", joined_clients);
        return;
    }

    server->phase = SERVER_PHASE_RUNNING;
    server_broadcast_info(server, "all players ready, game starting");
    snprintf(message, sizeof(message), "%s|%s\n",
             PROTOCOL_MSG_PROMPT, server->game.current_prompt);
    server_broadcast_message(server, message);
    fprintf(stderr, "server: game starting with %d players\n", joined_clients);
}

static int server_collect_joined_players(const server_state_t *server,
                                         int *player_ids,
                                         char usernames[][PROTOCOL_MAX_USERNAME_LEN + 1]) {
    int count;
    size_t i;

    count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!server->clients[i].active || !server->clients[i].joined) {
            continue;
        }

        player_ids[count] = server->clients[i].player_id;
        strncpy(usernames[count], server->clients[i].username,
                PROTOCOL_MAX_USERNAME_LEN);
        usernames[count][PROTOCOL_MAX_USERNAME_LEN] = '\0';
        count++;
    }

    return count;
}

static void server_print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s [port]\n", program_name);
}

int main(int argc, char *argv[]) {
    const char *port_text = STRINGIFY(PORT);

    if (argc > 2) {
        server_print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        port_text = argv[1];
    }

    return server_run(port_text);
}
