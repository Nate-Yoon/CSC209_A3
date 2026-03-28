/*
 * server.c
 *
 * Purpose:
 * Main authoritative server for the CSC209 A3 multiplayer terminal game.
 * This file owns TCP listener setup, select()-based socket multiplexing,
 * line-buffered client I/O, message dispatch, and disconnect cleanup.
 *
 * Current scope:
 * Networking and transport only. Gameplay state, per-round bookkeeping,
 * and phase transitions live behind `game.*` and `round.*`.
 */

#define _POSIX_C_SOURCE 200112L

#include "server.h"

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
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
static void server_reset_client_slot(server_client_t *client);
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
                                    size_t client_index);
static int server_handle_submit_line(server_state_t *server,
                                     size_t client_index,
                                     const char *line);
static void server_maybe_start_game(server_state_t *server);
static bool server_get_select_timeout(const server_state_t *server,
                                      struct timeval *timeout_out);
static void server_enforce_prompt_deadline(server_state_t *server);
static void server_handle_phase_change(server_state_t *server);
static void server_announce_round_start(server_state_t *server);
static void server_broadcast_round_results(server_state_t *server);
static const game_player_t *server_get_client_player(const server_state_t *server,
                                                     size_t client_index);
static bool server_client_has_joined(const server_client_t *client);
static int server_send_to_client(const server_client_t *client, const char *message);
static int server_send_error_and_close(server_state_t *server,
                                       size_t client_index,
                                       const char *reason);
static int server_broadcast_message(const server_state_t *server,
                                    const char *message);
static int server_broadcast_info(const server_state_t *server, const char *text);
static void server_broadcast_lobby_status(const server_state_t *server);
static void server_seed_rng_once(void);
static void server_print_usage(const char *program_name);
static void server_ignore_sigpipe(void);

void server_state_init(server_state_t *server) {
    size_t i;

    if (server == NULL) {
        return;
    }

    server_ignore_sigpipe();
    server_seed_rng_once();
    server->listen_fd = -1;
    server->next_player_id = 1;
    server->active_clients = 0;
    game_state_init(&server->game);

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        server_reset_client_slot(&server->clients[i]);
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
        struct timeval timeout;
        struct timeval *timeout_ptr;
        int max_fd;
        int ready_count;
        size_t i;

        FD_ZERO(&read_fds);
        FD_SET(server->listen_fd, &read_fds);
        max_fd = server->listen_fd;
        timeout_ptr = NULL;

        for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
            if (!server->clients[i].active) {
                continue;
            }

            FD_SET(server->clients[i].fd, &read_fds);
            if (server->clients[i].fd > max_fd) {
                max_fd = server->clients[i].fd;
            }
        }

        if (server_get_select_timeout(server, &timeout)) {
            timeout_ptr = &timeout;
        }

        ready_count = select(max_fd + 1, &read_fds, NULL, NULL, timeout_ptr);
        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("server: select");
            return EXIT_FAILURE;
        }

        if (ready_count == 0) {
            server_enforce_prompt_deadline(server);
            continue;
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

    if (server->game.phase != GAME_PHASE_LOBBY) {
        if (protocol_format_error(message, sizeof(message), "game already started") >= 0) {
            server_send_to_client(&(server_client_t){ .fd = client_fd }, message);
        }
        close(client_fd);
        fprintf(stderr, "server: rejected connection because game already started\n");
        return;
    }

    free_slot = server_find_free_slot(server);
    if (free_slot < 0) {
        if (protocol_format_error(message, sizeof(message), "lobby is full") >= 0) {
            server_send_to_client(&(server_client_t){ .fd = client_fd }, message);
        }
        close(client_fd);
        fprintf(stderr, "server: rejecting connection because lobby is full\n");
        return;
    }

    server->clients[free_slot].fd = client_fd;
    server->clients[free_slot].active = true;
    server->active_clients++;

    fprintf(stderr, "server: accepted client into slot %zd (fd=%d)\n",
            free_slot, client_fd);
}

static void server_reset_client_slot(server_client_t *client) {
    if (client == NULL) {
        return;
    }

    client->fd = -1;
    client->active = false;
    client->player_id = 0;
    client->input_buffer[0] = '\0';
    client->input_len = 0;
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
    int player_id;

    fd = server->clients[client_index].fd;
    player_id = server->clients[client_index].player_id;

    if (fd >= 0) {
        close(fd);
    }

    fprintf(stderr, "server: removed client in slot %zu", client_index);
    if (reason != NULL && *reason != '\0') {
        fprintf(stderr, " (%s)", reason);
    }
    fprintf(stderr, "\n");

    server_reset_client_slot(&server->clients[client_index]);

    if (server->active_clients > 0) {
        server->active_clients--;
    }

    if (player_id > 0) {
        game_handle_disconnect(&server->game, player_id);
        if (game_advance_phase_if_ready(&server->game)) {
            server_handle_phase_change(server);
        }
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
    server_enforce_prompt_deadline(server);

    switch (protocol_identify_message(line)) {
        case PROTOCOL_TYPE_JOIN:
            return server_handle_join_line(server, client_index, line);
        case PROTOCOL_TYPE_READY:
            return server_handle_ready_line(server, client_index);
        case PROTOCOL_TYPE_SUBMIT:
            return server_handle_submit_line(server, client_index, line);
        default:
            if (server_send_to_client(&server->clients[client_index],
                                      PROTOCOL_MSG_ERROR "|unsupported message\n") != 0) {
                server_remove_client(server, client_index, "send failure");
                return -1;
            }
            fprintf(stderr, "server: unsupported message from slot %zu: %s",
                    client_index, line);
            return 0;
    }
}

static int server_handle_join_line(server_state_t *server,
                                   size_t client_index,
                                   const char *line) {
    server_client_t *client;
    const game_player_t *player;
    char username[PROTOCOL_MAX_USERNAME_LEN + 1];
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    int player_id;
    game_action_result_t result;

    client = &server->clients[client_index];

    if (!protocol_parse_join_username(line, username, sizeof(username))) {
        return server_send_error_and_close(server, client_index,
                                           "invalid JOIN format");
    }

    if (!protocol_username_is_valid(username)) {
        return server_send_error_and_close(server, client_index,
                                           "invalid username");
    }

    player_id = client->player_id;
    if (player_id == 0) {
        player_id = server->next_player_id;
    }

    result = game_handle_join(&server->game, player_id, username);
    if (result != GAME_ACTION_OK) {
        if (result == GAME_ACTION_ALREADY_JOINED) {
            server_send_to_client(client, PROTOCOL_MSG_ERROR "|already joined\n");
            return 0;
        }

        if (result == GAME_ACTION_INVALID_STATE) {
            return server_send_error_and_close(server, client_index,
                                               "game already started");
        }

        return server_send_error_and_close(server, client_index,
                                           game_action_result_message(result));
    }

    if (client->player_id == 0) {
        client->player_id = player_id;
        server->next_player_id++;
    }

    player = server_get_client_player(server, client_index);
    if (player == NULL) {
        return server_send_error_and_close(server, client_index,
                                           "server state error");
    }

    if (protocol_format_welcome(message, sizeof(message), client->player_id) >= 0) {
        if (server_send_to_client(client, message) != 0) {
            server_remove_client(server, client_index, "send failure");
            return -1;
        }
    }

    snprintf(message, sizeof(message),
             "%s joined the lobby (%d/%d players)",
             player->username,
             game_count_joined_players(&server->game),
             PROTOCOL_MAX_PLAYERS);
    server_broadcast_info(server, message);

    fprintf(stderr, "server: slot %zu joined as %s (player_id=%d)\n",
            client_index, player->username, client->player_id);
    server_broadcast_lobby_status(server);
    return 0;
}

static int server_handle_ready_line(server_state_t *server,
                                    size_t client_index) {
    server_client_t *client;
    const game_player_t *player;
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    game_action_result_t result;

    client = &server->clients[client_index];
    if (!server_client_has_joined(client)) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    result = game_handle_ready(&server->game, client->player_id);
    if (result != GAME_ACTION_OK) {
        if (result == GAME_ACTION_ALREADY_READY) {
            server_send_to_client(client, PROTOCOL_MSG_ERROR "|already ready\n");
            return 0;
        }

        if (result == GAME_ACTION_INVALID_STATE) {
            server_send_to_client(client, PROTOCOL_MSG_ERROR "|game already started\n");
            return 0;
        }

        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    player = server_get_client_player(server, client_index);
    if (player == NULL) {
        return server_send_error_and_close(server, client_index,
                                           "server state error");
    }

    snprintf(message, sizeof(message), "%s is ready", player->username);
    server_broadcast_info(server, message);
    server_broadcast_lobby_status(server);
    server_maybe_start_game(server);

    fprintf(stderr, "server: slot %zu marked ready (%s)\n",
            client_index, player->username);
    return 0;
}

static int server_handle_submit_line(server_state_t *server,
                                     size_t client_index,
                                     const char *line) {
    server_client_t *client;
    const game_player_t *player;
    const round_state_t *round;
    char submission[PROTOCOL_MAX_SUBMISSION_LEN + 1];
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    game_action_result_t result;

    client = &server->clients[client_index];

    if (!server_client_has_joined(client)) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    if (!protocol_parse_submit_text(line, submission, sizeof(submission))) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|invalid SUBMIT format\n");
        return 0;
    }

    if (!protocol_submission_is_valid(submission)) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|invalid submission\n");
        return 0;
    }

    result = game_handle_submit(&server->game, client->player_id, submission);
    if (result != GAME_ACTION_OK) {
        if (result == GAME_ACTION_INVALID_STATE) {
            server_send_to_client(client,
                                  PROTOCOL_MSG_ERROR "|not accepting submissions\n");
            return 0;
        }

        if (result == GAME_ACTION_ALREADY_SUBMITTED) {
            server_send_to_client(client, PROTOCOL_MSG_ERROR "|submission rejected\n");
            return 0;
        }

        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    if (server_send_to_client(client, PROTOCOL_MSG_INFO "|submission received\n") != 0) {
        server_remove_client(server, client_index, "send failure");
        return -1;
    }

    round = game_get_current_round(&server->game);
    if (round != NULL) {
        snprintf(message, sizeof(message), "submission received (%d/%d)",
                 round->submission_count, round->participant_count);
        server_broadcast_info(server, message);
    }

    player = server_get_client_player(server, client_index);
    if (player != NULL) {
        fprintf(stderr, "server: received submission from slot %zu (%s)\n",
                client_index, player->username);
    }

    if (game_advance_phase_if_ready(&server->game)) {
        server_handle_phase_change(server);
    }

    return 0;
}

static void server_maybe_start_game(server_state_t *server) {
    if (!game_can_start(&server->game)) {
        return;
    }

    if (!game_start(&server->game)) {
        server_broadcast_info(server,
                              "game could not start because question_prompts.txt could not be loaded");
        fprintf(stderr, "server: game_start failed during prompt-bank setup\n");
        return;
    }

    server_broadcast_info(server, "all players ready, game starting");
    server_announce_round_start(server);
    fprintf(stderr, "server: game starting with %d players\n",
            game_count_joined_players(&server->game));
}

static bool server_get_select_timeout(const server_state_t *server,
                                      struct timeval *timeout_out) {
    time_t deadline;
    time_t now;

    if (server == NULL || timeout_out == NULL) {
        return false;
    }

    deadline = game_get_submission_deadline(&server->game);
    if (deadline == 0) {
        return false;
    }

    now = time(NULL);
    timeout_out->tv_usec = 0;
    if (deadline <= now) {
        timeout_out->tv_sec = 0;
        return true;
    }

    timeout_out->tv_sec = deadline - now;
    return true;
}

static void server_enforce_prompt_deadline(server_state_t *server) {
    int fallback_count;

    if (server == NULL) {
        return;
    }

    fallback_count = game_apply_submission_timeout(&server->game, time(NULL));
    if (fallback_count > 0) {
        server_broadcast_info(server,
                              "submission time expired; fallback answers were used for missing players");
    }

    if (server->game.phase == GAME_PHASE_RESULTS) {
        server_handle_phase_change(server);
    }
}

static void server_handle_phase_change(server_state_t *server) {
    if (server == NULL) {
        return;
    }

    if (server->game.phase == GAME_PHASE_RESULTS) {
        server_broadcast_round_results(server);
        game_finish_round(&server->game);
    }
}

static void server_announce_round_start(server_state_t *server) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    size_t i;

    if (server == NULL) {
        return;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const char *prompt_text;

        if (!server->clients[i].active || !server_client_has_joined(&server->clients[i])) {
            continue;
        }

        prompt_text = game_get_player_prompt(&server->game, server->clients[i].player_id);
        if (prompt_text == NULL) {
            continue;
        }

        if (protocol_format_prompt(message, sizeof(message), prompt_text) < 0) {
            continue;
        }

        if (server_send_to_client(&server->clients[i], message) != 0) {
            server_remove_client(server, i, "send failure");
        }
    }
}

static void server_broadcast_round_results(server_state_t *server) {
    const round_state_t *round;
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    char winner[PROTOCOL_MAX_USERNAME_LEN + 1];
    size_t i;

    round = game_get_current_round(&server->game);
    if (round == NULL) {
        return;
    }

    server_broadcast_info(server, "all submissions received");

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(&server->game, i);
        const char *submission = round_get_player_submission(round, i);

        if (player == NULL || !player->joined || submission == NULL || submission[0] == '\0') {
            continue;
        }

        if (protocol_format_result(message, sizeof(message),
                                   player->username,
                                   submission) >= 0) {
            server_broadcast_message(server, message);
        }
    }

    if (game_pick_round_winner(&server->game, winner, sizeof(winner))) {
        if (protocol_format_winner(message, sizeof(message), winner) >= 0) {
            server_broadcast_message(server, message);
        }
    }

    server_broadcast_info(server, "round over");
}

static const game_player_t *server_get_client_player(const server_state_t *server,
                                                     size_t client_index) {
    if (server == NULL || client_index >= PROTOCOL_MAX_PLAYERS) {
        return NULL;
    }

    if (!server_client_has_joined(&server->clients[client_index])) {
        return NULL;
    }

    return game_get_player(&server->game, server->clients[client_index].player_id);
}

static bool server_client_has_joined(const server_client_t *client) {
    return client != NULL && client->player_id > 0;
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
            if (errno == EINTR) {
                continue;
            }
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

    if (server == NULL || message == NULL) {
        return -1;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!server->clients[i].active || !server_client_has_joined(&server->clients[i])) {
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

static void server_broadcast_lobby_status(const server_state_t *server) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    snprintf(message, sizeof(message),
             "lobby status: %d joined, %d ready, need at least %d ready to start",
             game_count_joined_players(&server->game),
             game_count_ready_players(&server->game),
             PROTOCOL_MIN_PLAYERS);
    server_broadcast_info(server, message);
}

static void server_seed_rng_once(void) {
    static int seeded = 0;

    if (seeded) {
        return;
    }

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    seeded = 1;
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

static void server_ignore_sigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}
