#ifndef SERVER_H
#define SERVER_H

/*
 * server.h
 *
 * Purpose:
 * Public server-facing declarations for the CSC209 A3 project.
 * This header describes the stable server-side state used by the authoritative
 * process that owns the listening socket, client table, and select()-driven
 * event loop.
 *
 * Current scope:
 * Listener setup, connection management, line-buffered socket reads, and
 * message dispatch. The server owns networking while `game.*` owns gameplay
 * state and phase transitions.
 */

#include <stdbool.h>
#include <stddef.h>

#include "game.h"
#include "protocol.h"

typedef struct {
    int fd;
    bool active;
    int player_id;
    char input_buffer[PROTOCOL_LINE_BUFFER_SIZE];
    size_t input_len;
} server_client_t;

typedef struct {
    int listen_fd;
    int next_player_id;
    int active_clients;
    game_state_t game;
    server_client_t clients[PROTOCOL_MAX_PLAYERS];
} server_state_t;

void server_state_init(server_state_t *server);
int server_run(const char *port_text);

#endif
