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

#include "game_view.h"
#include "server.h"

#include <fcntl.h>
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
    SERVER_READ_CHUNK_SIZE = 256,
    SERVER_STAGE_HOLD_SECONDS = 4,
    SERVER_TEXT_GROUP_DELAY_USEC = 1000000
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
static int server_set_nonblocking(int fd);
static void server_handle_client_readable(server_state_t *server,
                                          size_t client_index);
static void server_handle_client_writable(server_state_t *server,
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
static int server_handle_title_line(server_state_t *server,
                                    size_t client_index,
                                    const char *line);
static int server_handle_vote_line(server_state_t *server,
                                   size_t client_index,
                                   const char *line);
static void server_maybe_start_game(server_state_t *server);
static bool server_get_select_timeout(const server_state_t *server,
                                      struct timeval *timeout_out);
static void server_enforce_phase_deadline(server_state_t *server);
static void server_run_pending_action_if_due(server_state_t *server);
static void server_schedule_pending_action(server_state_t *server,
                                           server_pending_action_t action,
                                           time_t start_time);
static void server_handle_phase_change(server_state_t *server);
static void server_run_question_prompt_action(server_state_t *server,
                                              game_view_sink_t *view,
                                              time_t now);
static void server_run_title_prompt_action(server_state_t *server,
                                           game_view_sink_t *view,
                                           time_t now);
static void server_run_voting_prompt_action(server_state_t *server,
                                            game_view_sink_t *view,
                                            time_t now);
static void server_run_round_results_action(server_state_t *server,
                                            game_view_sink_t *view,
                                            time_t now);
static void server_run_scoreboard_action(server_state_t *server,
                                         game_view_sink_t *view,
                                         time_t now);
static void server_run_final_scoreboard_action(server_state_t *server,
                                               game_view_sink_t *view,
                                               time_t now);
static void server_run_game_over_action(server_state_t *server);
static void server_pause_text_group(void);
static void server_view_send_to_player(void *context,
                                       int player_id,
                                       const char *message);
static void server_view_broadcast_text(void *context, const char *text);
static void server_view_pause_text_group(void *context);
static void server_init_game_view(game_view_sink_t *sink, server_state_t *server);
static ssize_t server_find_client_index_by_player_id(const server_state_t *server,
                                                     int player_id);
static const game_player_t *server_get_client_player(const server_state_t *server,
                                                     size_t client_index);
static bool server_client_has_joined(const server_client_t *client);
static int server_send_to_client(server_client_t *client, const char *message);
static int server_try_flush_output(server_client_t *client);
static void server_log_outgoing_message(int fd, const char *message);
static int server_send_transient_message(int fd, const char *message);
static int server_send_error_and_close(server_state_t *server,
                                       size_t client_index,
                                       const char *reason);
static int server_broadcast_message(server_state_t *server,
                                    const char *message);
static int server_broadcast_lobby_event(server_state_t *server, const char *text);
static int server_broadcast_lobby_roster(server_state_t *server);
static int server_broadcast_round_text(server_state_t *server, const char *text);
static int server_broadcast_game_event(server_state_t *server, const char *text);
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
    server->pending_action_at = 0;
    server->pending_action = SERVER_PENDING_NONE;
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
        fd_set write_fds;
        struct timeval timeout;
        struct timeval *timeout_ptr;
        int max_fd;
        int select_result;
        int any_ready;
        size_t i;

        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(server->listen_fd, &read_fds);
        max_fd = server->listen_fd;
        timeout_ptr = NULL;

        for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
            if (!server->clients[i].active) {
                continue;
            }

            FD_SET(server->clients[i].fd, &read_fds);
            if (server->clients[i].output_len > 0) {
                FD_SET(server->clients[i].fd, &write_fds);
            }
            if (server->clients[i].fd > max_fd) {
                max_fd = server->clients[i].fd;
            }
        }

        if (server_get_select_timeout(server, &timeout)) {
            timeout_ptr = &timeout;
        }

        select_result = select(max_fd + 1, &read_fds, &write_fds, NULL, timeout_ptr);
        if (select_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("server: select");
            return EXIT_FAILURE;
        }

        any_ready = FD_ISSET(server->listen_fd, &read_fds);
        for (i = 0; i < PROTOCOL_MAX_PLAYERS && !any_ready; i++) {
            if (!server->clients[i].active) {
                continue;
            }

            if (FD_ISSET(server->clients[i].fd, &read_fds) ||
                FD_ISSET(server->clients[i].fd, &write_fds)) {
                any_ready = 1;
            }
        }

        if (!any_ready) {
            server_run_pending_action_if_due(server);
            server_enforce_phase_deadline(server);
            continue;
        }

        if (FD_ISSET(server->listen_fd, &read_fds)) {
            server_accept_client(server);
        }

        for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
            if (!server->clients[i].active) {
                continue;
            }

            if (FD_ISSET(server->clients[i].fd, &read_fds)) {
                server_handle_client_readable(server, i);
            }

            if (!server->clients[i].active) {
                continue;
            }

            if (FD_ISSET(server->clients[i].fd, &write_fds)) {
                server_handle_client_writable(server, i);
            }
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

    if (server_set_nonblocking(client_fd) != 0) {
        perror("server: fcntl");
        close(client_fd);
        return;
    }

    if (server->game.phase != GAME_PHASE_LOBBY) {
        if (protocol_format_error(message, sizeof(message), "game already started") >= 0) {
            server_send_transient_message(client_fd, message);
        }
        close(client_fd);
        fprintf(stderr, "server: rejected connection because game already started\n");
        return;
    }

    free_slot = server_find_free_slot(server);
    if (free_slot < 0) {
        if (protocol_format_error(message, sizeof(message), "lobby is full") >= 0) {
            server_send_transient_message(client_fd, message);
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
    client->output_buffer[0] = '\0';
    client->output_len = 0;
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
    game_phase_t phase_before_disconnect;

    fd = server->clients[client_index].fd;
    player_id = server->clients[client_index].player_id;
    phase_before_disconnect = server->game.phase;

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
        if (phase_before_disconnect != GAME_PHASE_LOBBY &&
            phase_before_disconnect != GAME_PHASE_OVER &&
            server->game.phase == GAME_PHASE_OVER) {
            game_view_sink_t view;

            server->pending_action = SERVER_PENDING_NONE;
            server->pending_action_at = 0;
            server_init_game_view(&view, server);
            game_view_broadcast_stage_banner(&view, "Game Over");
            server_broadcast_game_event(server,
                                        "game ended because too few players remain connected");
            return;
        }

        if (game_advance_phase_if_ready(&server->game)) {
            server_handle_phase_change(server);
        }
    }
}

static int server_set_nonblocking(int fd) {
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    return 0;
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
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

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

static void server_handle_client_writable(server_state_t *server,
                                          size_t client_index) {
    if (server_try_flush_output(&server->clients[client_index]) != 0) {
        server_remove_client(server, client_index, "send failure");
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
    server_run_pending_action_if_due(server);
    server_enforce_phase_deadline(server);

    switch (protocol_identify_message(line)) {
        case PROTOCOL_TYPE_JOIN:
            return server_handle_join_line(server, client_index, line);
        case PROTOCOL_TYPE_READY:
            return server_handle_ready_line(server, client_index);
        case PROTOCOL_TYPE_SUBMIT:
            return server_handle_submit_line(server, client_index, line);
        case PROTOCOL_TYPE_TITLE:
            return server_handle_title_line(server, client_index, line);
        case PROTOCOL_TYPE_VOTE:
            return server_handle_vote_line(server, client_index, line);
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
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|invalid username\n");
        return 0;
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

        server_send_to_client(client,
                              PROTOCOL_MSG_ERROR "|invalid username\n");
        return 0;
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
    server_broadcast_lobby_event(server, message);

    fprintf(stderr, "server: slot %zu joined as %s (player_id=%d)\n",
            client_index, player->username, client->player_id);
    if (game_count_joined_players(&server->game) >= PROTOCOL_MIN_PLAYERS) {
        server_broadcast_lobby_roster(server);
    }
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

    snprintf(message, sizeof(message), "%s is ready (%d/%d ready)",
             player->username,
             game_count_ready_players(&server->game),
             game_count_joined_players(&server->game));
    server_broadcast_lobby_event(server, message);
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
    char submission[PROTOCOL_MAX_SUBMISSION_LEN + 1];
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

    if (!protocol_player_text_is_valid(submission, PROTOCOL_MAX_SUBMISSION_LEN)) {
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

static int server_handle_title_line(server_state_t *server,
                                    size_t client_index,
                                    const char *line) {
    server_client_t *client;
    const game_player_t *player;
    char title_text[PROTOCOL_MAX_SUBMISSION_LEN + 1];
    game_action_result_t result;

    client = &server->clients[client_index];

    if (!server_client_has_joined(client)) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    if (!protocol_parse_title_text(line, title_text, sizeof(title_text))) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|invalid TITLE format\n");
        return 0;
    }

    if (!protocol_player_text_is_valid(title_text, PROTOCOL_MAX_SUBMISSION_LEN)) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|invalid title\n");
        return 0;
    }

    result = game_handle_rewrite(&server->game, client->player_id, title_text);
    if (result != GAME_ACTION_OK) {
        if (result == GAME_ACTION_INVALID_STATE) {
            server_send_to_client(client, PROTOCOL_MSG_ERROR "|not accepting titles\n");
            return 0;
        }

        if (result == GAME_ACTION_ALREADY_REWRITTEN) {
            server_send_to_client(client, PROTOCOL_MSG_ERROR "|title rejected\n");
            return 0;
        }

        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    player = server_get_client_player(server, client_index);
    if (player != NULL) {
        fprintf(stderr, "server: received title from slot %zu (%s)\n",
                client_index, player->username);
    }

    if (game_advance_phase_if_ready(&server->game)) {
        server_handle_phase_change(server);
    }

    return 0;
}

static int server_handle_vote_line(server_state_t *server,
                                   size_t client_index,
                                   const char *line) {
    server_client_t *client;
    const game_player_t *player;
    int option_number;
    int target_player_id;
    game_action_result_t result;

    client = &server->clients[client_index];

    if (!server_client_has_joined(client)) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    if (!protocol_parse_vote_target(line, &option_number)) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|invalid vote format\n");
        return 0;
    }

    target_player_id = game_get_vote_target_player_id_at(&server->game, option_number);
    if (target_player_id <= 0) {
        server_send_to_client(client, PROTOCOL_MSG_ERROR "|choose a valid vote option\n");
        return 0;
    }

    result = game_handle_vote(&server->game, client->player_id, target_player_id);
    if (result != GAME_ACTION_OK) {
        if (result == GAME_ACTION_INVALID_STATE) {
            server_send_to_client(client, PROTOCOL_MSG_ERROR "|not accepting votes\n");
            return 0;
        }

        if (result == GAME_ACTION_ALREADY_VOTED) {
            server_send_to_client(client, PROTOCOL_MSG_ERROR "|vote rejected\n");
            return 0;
        }

        if (result == GAME_ACTION_INVALID_VOTE_TARGET) {
            server_send_to_client(client,
                                  PROTOCOL_MSG_ERROR "|you cannot vote for the entry you titled\n");
            return 0;
        }

        server_send_to_client(client, PROTOCOL_MSG_ERROR "|join first\n");
        return 0;
    }

    player = server_get_client_player(server, client_index);
    if (player != NULL) {
        fprintf(stderr, "server: received vote from slot %zu (%s)\n",
                client_index, player->username);
    }

    if (game_advance_phase_if_ready(&server->game)) {
        server_handle_phase_change(server);
    }

    return 0;
}

static void server_maybe_start_game(server_state_t *server) {
    game_view_sink_t view;
    time_t now;

    if (!game_can_start(&server->game)) {
        return;
    }

    if (!game_start(&server->game)) {
        server_broadcast_game_event(server,
                                    "game could not start because question_prompts.txt could not be loaded");
        fprintf(stderr, "server: game_start failed during prompt-bank setup\n");
        return;
    }

    server_broadcast_game_event(server, "all players ready, game starting");
    server_init_game_view(&view, server);
    game_view_broadcast_round_intro(&server->game, &view);
    now = time(NULL);
    server_schedule_pending_action(server,
                                  SERVER_PENDING_QUESTION_PROMPT,
                                  now + SERVER_STAGE_HOLD_SECONDS);
    fprintf(stderr, "server: game starting with %d players\n",
            game_count_joined_players(&server->game));
}

static bool server_get_select_timeout(const server_state_t *server,
                                      struct timeval *timeout_out) {
    time_t deadline;
    time_t pending_action_at;
    time_t now;

    if (server == NULL || timeout_out == NULL) {
        return false;
    }

    deadline = game_get_phase_deadline(&server->game);
    pending_action_at = server->pending_action_at;
    if (pending_action_at != 0 &&
        (deadline == 0 || pending_action_at < deadline)) {
        deadline = pending_action_at;
    }
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

static void server_enforce_phase_deadline(server_state_t *server) {
    game_phase_t phase_before;
    int timeout_fill_count;

    if (server == NULL) {
        return;
    }

    phase_before = server->game.phase;
    timeout_fill_count = game_apply_phase_timeout(&server->game, time(NULL));
    if (timeout_fill_count > 0) {
        if (phase_before == GAME_PHASE_PROMPT) {
            server_broadcast_game_event(server,
                                        "submission time expired; fallback answers were used for missing players");
        } else if (phase_before == GAME_PHASE_REWRITE) {
            server_broadcast_game_event(server,
                                        "title time expired; empty titles were stored for missing players");
        } else if (phase_before == GAME_PHASE_VOTING) {
            server_broadcast_game_event(server,
                                        "voting time expired; missing votes were counted as abstaining");
        }
    }

    if (server->game.phase != phase_before) {
        server_handle_phase_change(server);
    }
}

static void server_run_pending_action_if_due(server_state_t *server) {
    game_view_sink_t view;
    time_t now;

    if (server == NULL || server->pending_action == SERVER_PENDING_NONE) {
        return;
    }

    now = time(NULL);
    if (server->pending_action_at != 0 && now < server->pending_action_at) {
        return;
    }

    server_init_game_view(&view, server);

    switch (server->pending_action) {
        case SERVER_PENDING_QUESTION_PROMPT:
            server_run_question_prompt_action(server, &view, now);
            break;
        case SERVER_PENDING_TITLE_PROMPT:
            server_run_title_prompt_action(server, &view, now);
            break;
        case SERVER_PENDING_VOTING_PROMPT:
            server_run_voting_prompt_action(server, &view, now);
            break;
        case SERVER_PENDING_ROUND_RESULTS:
            server_run_round_results_action(server, &view, now);
            break;
        case SERVER_PENDING_SCOREBOARD:
            server_run_scoreboard_action(server, &view, now);
            break;
        case SERVER_PENDING_FINAL_SCOREBOARD:
            server_run_final_scoreboard_action(server, &view, now);
            break;
        case SERVER_PENDING_GAME_OVER:
            server_run_game_over_action(server);
            break;
        case SERVER_PENDING_NONE:
            break;
    }
}

static void server_schedule_pending_action(server_state_t *server,
                                           server_pending_action_t action,
                                           time_t start_time) {
    if (server == NULL) {
        return;
    }

    server->pending_action = action;
    server->pending_action_at = start_time;
}

static void server_run_question_prompt_action(server_state_t *server,
                                              game_view_sink_t *view,
                                              time_t now) {
    server->pending_action = SERVER_PENDING_NONE;
    server->pending_action_at = 0;
    game_view_broadcast_stage_banner(view, "Question Round");
    game_start_prompt_window(&server->game, now);
    server_pause_text_group();
    game_view_announce_round_start(&server->game, view);
}

static void server_run_title_prompt_action(server_state_t *server,
                                           game_view_sink_t *view,
                                           time_t now) {
    server->pending_action = SERVER_PENDING_NONE;
    server->pending_action_at = 0;
    game_start_title_window(&server->game, now);
    game_view_send_title_prompts(&server->game, view);
}

static void server_run_voting_prompt_action(server_state_t *server,
                                            game_view_sink_t *view,
                                            time_t now) {
    time_t start_time;

    (void)now;
    server->pending_action = SERVER_PENDING_NONE;
    server->pending_action_at = 0;
    game_view_announce_voting_phase(&server->game, view);
    start_time = time(NULL);
    game_start_vote_window(&server->game, start_time);
}

static void server_run_round_results_action(server_state_t *server,
                                            game_view_sink_t *view,
                                            time_t now) {
    server->pending_action = SERVER_PENDING_NONE;
    server->pending_action_at = 0;
    game_view_broadcast_round_results(&server->game, view);
    if (game_is_final_round(&server->game)) {
        game_view_broadcast_stage_banner(view, "Final Scoreboard");
        server_schedule_pending_action(server,
                                      SERVER_PENDING_FINAL_SCOREBOARD,
                                      now + SERVER_STAGE_HOLD_SECONDS);
    } else {
        game_view_broadcast_stage_banner(view, "Scoreboard");
        server_schedule_pending_action(server,
                                      SERVER_PENDING_SCOREBOARD,
                                      now + SERVER_STAGE_HOLD_SECONDS);
    }
}

static void server_run_scoreboard_action(server_state_t *server,
                                         game_view_sink_t *view,
                                         time_t now) {
    server->pending_action = SERVER_PENDING_NONE;
    server->pending_action_at = 0;
    game_view_broadcast_scoreboard(&server->game, view, "Current scores:");
    if (!game_finish_round(&server->game)) {
        game_view_broadcast_stage_banner(view, "Game Over");
        server_broadcast_game_event(server, "game ended because the next round could not start");
        game_end(&server->game);
        return;
    }

    game_view_broadcast_round_intro(&server->game, view);
    server_schedule_pending_action(server,
                                  SERVER_PENDING_QUESTION_PROMPT,
                                  now + SERVER_STAGE_HOLD_SECONDS);
}

static void server_run_final_scoreboard_action(server_state_t *server,
                                               game_view_sink_t *view,
                                               time_t now) {
    server->pending_action = SERVER_PENDING_NONE;
    server->pending_action_at = 0;
    game_view_broadcast_scoreboard(&server->game, view, "Final scores:");
    server_pause_text_group();
    game_view_broadcast_final_winners(&server->game, view);
    game_finish_round(&server->game);
    game_view_broadcast_stage_banner(view, "Game Over");
    server_schedule_pending_action(server,
                                  SERVER_PENDING_GAME_OVER,
                                  now + SERVER_STAGE_HOLD_SECONDS);
}

static void server_run_game_over_action(server_state_t *server) {
    server->pending_action = SERVER_PENDING_NONE;
    server->pending_action_at = 0;
    server_broadcast_game_event(server, "Thanks for playing.");
}

static void server_handle_phase_change(server_state_t *server) {
    game_view_sink_t view;
    time_t now;

    if (server == NULL) {
        return;
    }

    now = time(NULL);
    server_init_game_view(&view, server);

    if (server->game.phase == GAME_PHASE_REWRITE) {
        game_view_broadcast_stage_banner(&view, "Title Trouble");
        server_schedule_pending_action(server,
                                      SERVER_PENDING_TITLE_PROMPT,
                                      now + SERVER_STAGE_HOLD_SECONDS);
        return;
    }

    if (server->game.phase == GAME_PHASE_VOTING) {
        game_view_broadcast_stage_banner(&view, "Vote Time");
        server_schedule_pending_action(server,
                                      SERVER_PENDING_VOTING_PROMPT,
                                      now + SERVER_STAGE_HOLD_SECONDS);
        return;
    }

    if (server->game.phase == GAME_PHASE_RESULTS) {
        server_schedule_pending_action(server,
                                      SERVER_PENDING_ROUND_RESULTS,
                                      now + SERVER_STAGE_HOLD_SECONDS);
    }
}

static void server_pause_text_group(void) {
    struct timespec delay;

    delay.tv_sec = SERVER_TEXT_GROUP_DELAY_USEC / 1000000;
    delay.tv_nsec = (long)(SERVER_TEXT_GROUP_DELAY_USEC % 1000000) * 1000L;
    nanosleep(&delay, NULL);
}

static void server_view_send_to_player(void *context,
                                       int player_id,
                                       const char *message) {
    server_state_t *server;
    ssize_t client_index;

    server = context;
    if (server == NULL || message == NULL) {
        return;
    }

    client_index = server_find_client_index_by_player_id(server, player_id);
    if (client_index < 0) {
        return;
    }

    if (server_send_to_client(&server->clients[client_index], message) != 0) {
        server_remove_client(server, (size_t)client_index, "send failure");
    }
}

static void server_view_broadcast_text(void *context, const char *text) {
    server_state_t *server;

    server = context;
    if (server == NULL || text == NULL) {
        return;
    }

    server_broadcast_round_text(server, text);
}

static void server_view_pause_text_group(void *context) {
    (void)context;
    server_pause_text_group();
}

static void server_init_game_view(game_view_sink_t *sink, server_state_t *server) {
    if (sink == NULL) {
        return;
    }

    sink->context = server;
    sink->send_to_player = server_view_send_to_player;
    sink->broadcast_text = server_view_broadcast_text;
    sink->pause_text_group = server_view_pause_text_group;
}

static ssize_t server_find_client_index_by_player_id(const server_state_t *server,
                                                     int player_id) {
    size_t i;

    if (server == NULL || player_id <= 0) {
        return -1;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!server->clients[i].active) {
            continue;
        }

        if (server->clients[i].player_id == player_id) {
            return (ssize_t)i;
        }
    }

    return -1;
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

static int server_send_to_client(server_client_t *client, const char *message) {
    size_t message_len;

    if (client == NULL || message == NULL || client->fd < 0) {
        return -1;
    }

    server_log_outgoing_message(client->fd, message);

    message_len = strlen(message);
    if (message_len > sizeof(client->output_buffer) - client->output_len) {
        return -1;
    }

    memcpy(client->output_buffer + client->output_len, message, message_len);
    client->output_len += message_len;
    client->output_buffer[client->output_len] = '\0';

    return server_try_flush_output(client);
}

static int server_try_flush_output(server_client_t *client) {
    while (client != NULL && client->output_len > 0) {
        ssize_t sent = send(client->fd, client->output_buffer, client->output_len, 0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }

            return -1;
        }

        if (sent == 0) {
            return -1;
        }

        if ((size_t)sent < client->output_len) {
            memmove(client->output_buffer,
                    client->output_buffer + sent,
                    client->output_len - (size_t)sent);
        }

        client->output_len -= (size_t)sent;
        client->output_buffer[client->output_len] = '\0';
    }

    return 0;
}

static void server_log_outgoing_message(int fd, const char *message) {
    const char *newline;

    if (fd < 0 || message == NULL) {
        return;
    }

    newline = strchr(message, '\n');
    if (newline == NULL) {
        fprintf(stderr, "server -> fd %d: %s\n", fd, message);
        return;
    }

    fprintf(stderr, "server -> fd %d: %.*s\n",
            fd,
            (int)(newline - message),
            message);
}

static int server_send_transient_message(int fd, const char *message) {
    size_t total_sent = 0;
    size_t message_len;

    if (fd < 0 || message == NULL) {
        return -1;
    }

    server_log_outgoing_message(fd, message);

    message_len = strlen(message);
    while (total_sent < message_len) {
        ssize_t sent = send(fd,
                            message + total_sent,
                            message_len - total_sent,
                            0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            return -1;
        }

        if (sent == 0) {
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

static int server_broadcast_message(server_state_t *server,
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
            server_remove_client(server, i, "send failure");
        }
    }

    return 0;
}

static int server_broadcast_lobby_event(server_state_t *server, const char *text) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    if (protocol_format_lobby_event(message, sizeof(message), text) < 0) {
        return -1;
    }

    return server_broadcast_message(server, message);
}

static int server_broadcast_lobby_roster(server_state_t *server) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    const char *usernames[PROTOCOL_MAX_PLAYERS];
    size_t username_count;
    size_t i;

    if (server == NULL) {
        return -1;
    }

    username_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(&server->game, i);

        if (player == NULL || !player->joined || !player->connected) {
            continue;
        }

        usernames[username_count] = player->username;
        username_count++;
    }

    if (username_count == 0) {
        return 0;
    }

    if (protocol_format_lobby_roster(message,
                                     sizeof(message),
                                     usernames,
                                     username_count) < 0) {
        return -1;
    }

    return server_broadcast_message(server, message);
}

static int server_broadcast_round_text(server_state_t *server, const char *text) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    if (protocol_format_round_text(message, sizeof(message), text) < 0) {
        return -1;
    }

    return server_broadcast_message(server, message);
}

static int server_broadcast_game_event(server_state_t *server, const char *text) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    if (protocol_format_game_event(message, sizeof(message), text) < 0) {
        return -1;
    }

    return server_broadcast_message(server, message);
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
