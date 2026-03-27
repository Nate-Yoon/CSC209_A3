/*
 * game.c
 *
 * Purpose:
 * Authoritative game-state module for the CSC209 A3 project.
 * This file owns lobby/player bookkeeping plus high-level phase transitions,
 * leaving server.c responsible for networking and message transport.
 */

#include "game.h"

#include <string.h>

static const char *const GAME_DEFAULT_PROMPT =
    "Share a harmless opinion that could look terrible online.";

static game_player_t *game_find_player(game_state_t *game, int player_id);
static const game_player_t *game_find_player_const(const game_state_t *game,
                                                   int player_id);
static int game_find_player_index(const game_state_t *game, int player_id);
static int game_find_open_player_slot(const game_state_t *game);
static bool game_username_is_unique(const game_state_t *game, const char *username);
static void game_reset_player(game_player_t *player);

void game_state_init(game_state_t *game) {
    if (game == NULL) {
        return;
    }

    game_state_reset(game);
}

void game_state_reset(game_state_t *game) {
    size_t i;

    if (game == NULL) {
        return;
    }

    game->phase = GAME_PHASE_LOBBY;
    game->round_number = 0;

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        game_reset_player(&game->players[i]);
    }

    round_state_reset(&game->current_round);
}

int game_count_joined_players(const game_state_t *game) {
    int count;
    size_t i;

    if (game == NULL) {
        return 0;
    }

    count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].joined && game->players[i].connected) {
            count++;
        }
    }

    return count;
}

int game_count_ready_players(const game_state_t *game) {
    int count;
    size_t i;

    if (game == NULL) {
        return 0;
    }

    count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].joined &&
            game->players[i].connected &&
            game->players[i].ready) {
            count++;
        }
    }

    return count;
}

bool game_can_start(const game_state_t *game) {
    int joined_players;

    if (game == NULL || game->phase != GAME_PHASE_LOBBY) {
        return false;
    }

    joined_players = game_count_joined_players(game);
    return joined_players >= PROTOCOL_MIN_PLAYERS &&
           joined_players <= PROTOCOL_MAX_PLAYERS &&
           game_count_ready_players(game) == joined_players;
}

game_action_result_t game_handle_join(game_state_t *game,
                                      int player_id,
                                      const char *username) {
    int player_index;
    game_player_t *player;

    if (game == NULL || username == NULL || !protocol_username_is_valid(username)) {
        return GAME_ACTION_INVALID_INPUT;
    }

    if (game->phase != GAME_PHASE_LOBBY) {
        return GAME_ACTION_INVALID_STATE;
    }

    if (player_id <= 0) {
        return GAME_ACTION_INVALID_INPUT;
    }

    player_index = game_find_player_index(game, player_id);
    if (player_index >= 0 && game->players[player_index].joined) {
        return GAME_ACTION_ALREADY_JOINED;
    }

    if (!game_username_is_unique(game, username)) {
        return GAME_ACTION_USERNAME_IN_USE;
    }

    if (player_index < 0) {
        player_index = game_find_open_player_slot(game);
        if (player_index < 0) {
            return GAME_ACTION_INVALID_STATE;
        }
    }

    player = &game->players[player_index];
    player->player_id = player_id;
    strncpy(player->username, username, sizeof(player->username) - 1);
    player->username[sizeof(player->username) - 1] = '\0';
    player->score = 0;
    player->connected = true;
    player->joined = true;
    player->ready = false;
    return GAME_ACTION_OK;
}

game_action_result_t game_handle_ready(game_state_t *game, int player_id) {
    game_player_t *player;

    if (game == NULL || player_id <= 0) {
        return GAME_ACTION_INVALID_INPUT;
    }

    player = game_find_player(game, player_id);
    if (player == NULL || !player->joined || !player->connected) {
        return GAME_ACTION_UNKNOWN_PLAYER;
    }

    if (game->phase != GAME_PHASE_LOBBY) {
        return GAME_ACTION_INVALID_STATE;
    }

    if (player->ready) {
        return GAME_ACTION_ALREADY_READY;
    }

    player->ready = true;
    return GAME_ACTION_OK;
}

game_action_result_t game_handle_submit(game_state_t *game,
                                        int player_id,
                                        const char *submission) {
    int player_index;

    if (game == NULL || submission == NULL || !protocol_submission_is_valid(submission)) {
        return GAME_ACTION_INVALID_INPUT;
    }

    if (game->phase != GAME_PHASE_PROMPT) {
        return GAME_ACTION_INVALID_STATE;
    }

    player_index = game_find_player_index(game, player_id);
    if (player_index < 0 ||
        !game->players[player_index].joined ||
        !game->players[player_index].connected) {
        return GAME_ACTION_UNKNOWN_PLAYER;
    }

    if (!game->current_round.players[player_index].active) {
        return GAME_ACTION_INVALID_STATE;
    }

    if (game->current_round.players[player_index].has_submitted) {
        return GAME_ACTION_ALREADY_SUBMITTED;
    }

    if (!round_record_submission(&game->current_round,
                                 (size_t)player_index,
                                 submission)) {
        return GAME_ACTION_INVALID_INPUT;
    }

    return GAME_ACTION_OK;
}

game_action_result_t game_handle_rewrite(game_state_t *game,
                                         int player_id,
                                         const char *rewrite_text) {
    int player_index;

    if (game == NULL ||
        rewrite_text == NULL ||
        !protocol_submission_is_valid(rewrite_text)) {
        return GAME_ACTION_INVALID_INPUT;
    }

    if (game->phase != GAME_PHASE_REWRITE) {
        return GAME_ACTION_INVALID_STATE;
    }

    player_index = game_find_player_index(game, player_id);
    if (player_index < 0 ||
        !game->players[player_index].joined ||
        !game->players[player_index].connected) {
        return GAME_ACTION_UNKNOWN_PLAYER;
    }

    if (!game->current_round.players[player_index].active) {
        return GAME_ACTION_INVALID_STATE;
    }

    if (game->current_round.players[player_index].has_rewritten) {
        return GAME_ACTION_ALREADY_REWRITTEN;
    }

    if (!round_record_rewrite(&game->current_round,
                              (size_t)player_index,
                              rewrite_text)) {
        return GAME_ACTION_INVALID_INPUT;
    }

    return GAME_ACTION_OK;
}

game_action_result_t game_handle_vote(game_state_t *game,
                                      int player_id,
                                      int target_player_id) {
    int voter_index;
    int target_index;

    if (game == NULL || player_id <= 0 || target_player_id <= 0) {
        return GAME_ACTION_INVALID_INPUT;
    }

    if (game->phase != GAME_PHASE_VOTING) {
        return GAME_ACTION_INVALID_STATE;
    }

    voter_index = game_find_player_index(game, player_id);
    target_index = game_find_player_index(game, target_player_id);
    if (voter_index < 0 ||
        !game->players[voter_index].joined ||
        !game->players[voter_index].connected) {
        return GAME_ACTION_UNKNOWN_PLAYER;
    }

    if (target_index < 0 ||
        !game->players[target_index].joined ||
        !game->current_round.players[target_index].active) {
        return GAME_ACTION_INVALID_VOTE_TARGET;
    }

    if (game->current_round.players[voter_index].has_voted) {
        return GAME_ACTION_ALREADY_VOTED;
    }

    if (!round_record_vote(&game->current_round,
                           (size_t)voter_index,
                           (size_t)target_index)) {
        return GAME_ACTION_INVALID_VOTE_TARGET;
    }

    return GAME_ACTION_OK;
}

bool game_start(game_state_t *game) {
    if (!game_can_start(game)) {
        return false;
    }

    return game_begin_round(game, GAME_DEFAULT_PROMPT);
}

bool game_begin_round(game_state_t *game, const char *prompt) {
    size_t i;
    int next_round_number;

    if (game == NULL || prompt == NULL) {
        return false;
    }

    if (game->phase != GAME_PHASE_LOBBY && game->phase != GAME_PHASE_RESULTS) {
        return false;
    }

    next_round_number = game->round_number + 1;
    if (!round_begin(&game->current_round, next_round_number, prompt)) {
        return false;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].joined && game->players[i].connected) {
            round_set_player_active(&game->current_round, i, true);
        }
    }

    game->round_number = next_round_number;
    game->phase = GAME_PHASE_PROMPT;
    return true;
}

bool game_advance_phase_if_ready(game_state_t *game) {
    if (game == NULL) {
        return false;
    }

    if (game->phase == GAME_PHASE_PROMPT &&
        round_all_submitted(&game->current_round)) {
        /*
         * TODO: Insert optional rewrite/twist and voting phase branching once
         * the real round rules are implemented.
         */
        game->phase = GAME_PHASE_RESULTS;
        return true;
    }

    if (game->phase == GAME_PHASE_REWRITE &&
        round_all_rewritten(&game->current_round)) {
        game->phase = GAME_PHASE_VOTING;
        return true;
    }

    if (game->phase == GAME_PHASE_VOTING &&
        round_all_voted(&game->current_round)) {
        game->phase = GAME_PHASE_RESULTS;
        return true;
    }

    return false;
}

bool game_finish_round(game_state_t *game) {
    if (game == NULL || game->phase != GAME_PHASE_RESULTS) {
        return false;
    }

    /*
     * TODO: Replace this single-round placeholder with real scoring and
     * multi-round progression once those rules are implemented.
     */
    game_end(game);
    return true;
}

void game_end(game_state_t *game) {
    if (game == NULL) {
        return;
    }

    game->phase = GAME_PHASE_OVER;
}

void game_handle_disconnect(game_state_t *game, int player_id) {
    int player_index;
    game_player_t *player;

    if (game == NULL || player_id <= 0) {
        return;
    }

    player_index = game_find_player_index(game, player_id);
    if (player_index < 0) {
        return;
    }

    player = &game->players[player_index];
    if (game->phase == GAME_PHASE_LOBBY) {
        game_reset_player(player);
        return;
    }

    player->connected = false;
    player->ready = false;
    if (game->current_round.active) {
        round_set_player_active(&game->current_round, (size_t)player_index, false);
    }
}

const game_player_t *game_get_player(const game_state_t *game, int player_id) {
    return game_find_player_const(game, player_id);
}

const game_player_t *game_get_player_at(const game_state_t *game, size_t player_index) {
    if (game == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return NULL;
    }

    return &game->players[player_index];
}

const round_state_t *game_get_current_round(const game_state_t *game) {
    if (game == NULL) {
        return NULL;
    }

    return &game->current_round;
}

bool game_pick_round_winner(const game_state_t *game,
                            char *username_out,
                            size_t username_out_size) {
    int winner_index;

    if (game == NULL || username_out == NULL || username_out_size == 0) {
        return false;
    }

    winner_index = round_pick_random_submitted_index(&game->current_round);
    if (winner_index < 0) {
        return false;
    }

    strncpy(username_out,
            game->players[winner_index].username,
            username_out_size - 1);
    username_out[username_out_size - 1] = '\0';
    return true;
}

const char *game_action_result_message(game_action_result_t result) {
    switch (result) {
        case GAME_ACTION_OK:
            return "ok";
        case GAME_ACTION_INVALID_INPUT:
            return "invalid request";
        case GAME_ACTION_INVALID_STATE:
            return "action not allowed right now";
        case GAME_ACTION_UNKNOWN_PLAYER:
            return "join first";
        case GAME_ACTION_ALREADY_JOINED:
            return "already joined";
        case GAME_ACTION_ALREADY_READY:
            return "already ready";
        case GAME_ACTION_USERNAME_IN_USE:
            return "username already in use";
        case GAME_ACTION_ALREADY_SUBMITTED:
            return "already submitted";
        case GAME_ACTION_ALREADY_REWRITTEN:
            return "already rewritten";
        case GAME_ACTION_ALREADY_VOTED:
            return "already voted";
        case GAME_ACTION_INVALID_VOTE_TARGET:
            return "invalid vote target";
    }

    return "request rejected";
}

static game_player_t *game_find_player(game_state_t *game, int player_id) {
    int i;

    if (game == NULL || player_id <= 0) {
        return NULL;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].player_id == player_id) {
            return &game->players[i];
        }
    }

    return NULL;
}

static const game_player_t *game_find_player_const(const game_state_t *game,
                                                   int player_id) {
    int i;

    if (game == NULL || player_id <= 0) {
        return NULL;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].player_id == player_id) {
            return &game->players[i];
        }
    }

    return NULL;
}

static int game_find_player_index(const game_state_t *game, int player_id) {
    int i;

    if (game == NULL || player_id <= 0) {
        return -1;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].player_id == player_id) {
            return i;
        }
    }

    return -1;
}

static int game_find_open_player_slot(const game_state_t *game) {
    int i;

    if (game == NULL) {
        return -1;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!game->players[i].joined) {
            return i;
        }
    }

    return -1;
}

static bool game_username_is_unique(const game_state_t *game, const char *username) {
    size_t i;

    if (game == NULL || username == NULL) {
        return false;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!game->players[i].joined) {
            continue;
        }

        if (strcmp(game->players[i].username, username) == 0) {
            return false;
        }
    }

    return true;
}

static void game_reset_player(game_player_t *player) {
    if (player == NULL) {
        return;
    }

    player->player_id = 0;
    player->username[0] = '\0';
    player->score = 0;
    player->connected = false;
    player->joined = false;
    player->ready = false;
}
