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

static game_player_t *game_find_player(game_state_t *game, int player_id);
static const game_player_t *game_find_player_const(const game_state_t *game,
                                                   int player_id);

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
        game->players[i].submission[0] = '\0';
        game->players[i].active = false;
        game->players[i].has_submitted = false;
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
        game->players[i].submission[0] = '\0';
        game->players[i].active = true;
        game->players[i].has_submitted = false;
    }

    return true;
}

bool game_submit(game_state_t *game, int player_id, const char *submission) {
    game_player_t *player;

    if (game == NULL || submission == NULL) {
        return false;
    }

    if (game->phase != GAME_PHASE_PROMPT || !protocol_submission_is_valid(submission)) {
        return false;
    }

    player = game_find_player(game, player_id);
    if (player == NULL || !player->active || player->has_submitted) {
        return false;
    }

    strncpy(player->submission, submission, sizeof(player->submission) - 1);
    player->submission[sizeof(player->submission) - 1] = '\0';
    player->has_submitted = true;
    game->submitted_count++;

    if (game_all_submitted(game)) {
        game->phase = GAME_PHASE_RESULTS;
    }

    return true;
}

bool game_all_submitted(const game_state_t *game) {
    int active_players;
    int submitted_players;
    int i;

    if (game == NULL || game->phase == GAME_PHASE_LOBBY) {
        return false;
    }

    active_players = 0;
    submitted_players = 0;
    for (i = 0; i < game->player_count; i++) {
        if (!game->players[i].active) {
            continue;
        }

        active_players++;
        if (game->players[i].has_submitted) {
            submitted_players++;
        }
    }

    return active_players > 0 && submitted_players == active_players;
}

const game_player_t *game_get_player(const game_state_t *game, int player_id) {
    return game_find_player_const(game, player_id);
}

static game_player_t *game_find_player(game_state_t *game, int player_id) {
    int i;

    if (game == NULL) {
        return NULL;
    }

    for (i = 0; i < game->player_count; i++) {
        if (game->players[i].player_id == player_id) {
            return &game->players[i];
        }
    }

    return NULL;
}

static const game_player_t *game_find_player_const(const game_state_t *game,
                                                   int player_id) {
    int i;

    if (game == NULL) {
        return NULL;
    }

    for (i = 0; i < game->player_count; i++) {
        if (game->players[i].player_id == player_id) {
            return &game->players[i];
        }
    }

    return NULL;
}
