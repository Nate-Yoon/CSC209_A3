/*
 * game.c
 *
 * Purpose:
 * Server-owned game-state module for the CSC209 A3 project.
 * This file contains the initial MVP round state so the networking layer can
 * transition into gameplay without owning all of the game rules itself.
 */

#include "game.h"

#include <string.h>

static const char *const GAME_DEFAULT_PROMPT =
    "Share a harmless opinion that could look terrible online.";

void game_state_init(game_state_t *game) {
    if (game == NULL) {
        return;
    }

    game_state_reset(game);
}

void game_state_reset(game_state_t *game) {
    int i;

    if (game == NULL) {
        return;
    }

    game->phase = GAME_PHASE_LOBBY;
    game->round_number = 0;
    game->player_count = 0;
    game->submitted_count = 0;
    game->current_prompt[0] = '\0';

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        game->players[i].player_id = 0;
        game->players[i].username[0] = '\0';
        game->players[i].active = false;
    }
}

bool game_can_start(int player_count) {
    return player_count >= PROTOCOL_MIN_PLAYERS &&
           player_count <= PROTOCOL_MAX_PLAYERS;
}

bool game_start(game_state_t *game,
                const int *player_ids,
                const char usernames[][PROTOCOL_MAX_USERNAME_LEN + 1],
                int player_count) {
    int i;

    if (game == NULL || player_ids == NULL || usernames == NULL) {
        return false;
    }

    if (!game_can_start(player_count)) {
        return false;
    }

    game_state_reset(game);
    game->phase = GAME_PHASE_PROMPT;
    game->round_number = 1;
    game->player_count = player_count;
    strncpy(game->current_prompt, GAME_DEFAULT_PROMPT,
            sizeof(game->current_prompt) - 1);
    game->current_prompt[sizeof(game->current_prompt) - 1] = '\0';

    for (i = 0; i < player_count; i++) {
        game->players[i].player_id = player_ids[i];
        strncpy(game->players[i].username, usernames[i],
                sizeof(game->players[i].username) - 1);
        game->players[i].username[sizeof(game->players[i].username) - 1] = '\0';
        game->players[i].active = true;
    }

    return true;
}
