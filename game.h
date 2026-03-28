#ifndef GAME_H
#define GAME_H

/*
 * game.h
 *
 * Purpose:
 * High-level game-state boundary for the CSC209 A3 server.
 * The server owns sockets and message transport, while this module owns the
 * authoritative lobby, player, phase, and round state used by gameplay.
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "protocol.h"
#include "round.h"

typedef enum {
    GAME_PHASE_LOBBY = 0,
    GAME_PHASE_PROMPT,
    GAME_PHASE_REWRITE,
    GAME_PHASE_VOTING,
    GAME_PHASE_RESULTS,
    GAME_PHASE_OVER
} game_phase_t;

typedef enum {
    GAME_ACTION_OK = 0,
    GAME_ACTION_INVALID_INPUT,
    GAME_ACTION_INVALID_STATE,
    GAME_ACTION_UNKNOWN_PLAYER,
    GAME_ACTION_ALREADY_JOINED,
    GAME_ACTION_ALREADY_READY,
    GAME_ACTION_USERNAME_IN_USE,
    GAME_ACTION_ALREADY_SUBMITTED,
    GAME_ACTION_ALREADY_REWRITTEN,
    GAME_ACTION_ALREADY_VOTED,
    GAME_ACTION_INVALID_VOTE_TARGET
} game_action_result_t;

typedef struct {
    int player_id;
    char username[PROTOCOL_MAX_USERNAME_LEN + 1];
    int score;
    bool connected;
    bool joined;
    bool ready;
} game_player_t;

typedef struct {
    game_phase_t phase;
    int round_number;
    game_player_t players[PROTOCOL_MAX_PLAYERS];
    round_state_t current_round;
} game_state_t;

void game_state_init(game_state_t *game);
void game_state_reset(game_state_t *game);
int game_count_joined_players(const game_state_t *game);
int game_count_ready_players(const game_state_t *game);
bool game_can_start(const game_state_t *game);
game_action_result_t game_handle_join(game_state_t *game,
                                      int player_id,
                                      const char *username);
game_action_result_t game_handle_ready(game_state_t *game, int player_id);
game_action_result_t game_handle_submit(game_state_t *game,
                                        int player_id,
                                        const char *submission);
game_action_result_t game_handle_rewrite(game_state_t *game,
                                         int player_id,
                                         const char *rewrite_text);
game_action_result_t game_handle_vote(game_state_t *game,
                                      int player_id,
                                      int target_player_id);
bool game_start(game_state_t *game);
bool game_begin_round(game_state_t *game);
bool game_advance_phase_if_ready(game_state_t *game);
bool game_finish_round(game_state_t *game);
void game_end(game_state_t *game);
void game_handle_disconnect(game_state_t *game, int player_id);
time_t game_get_phase_deadline(const game_state_t *game);
int game_apply_phase_timeout(game_state_t *game, time_t now);
const game_player_t *game_get_player(const game_state_t *game, int player_id);
const game_player_t *game_get_player_at(const game_state_t *game, size_t player_index);
const round_state_t *game_get_current_round(const game_state_t *game);
const char *game_get_player_prompt(const game_state_t *game, int player_id);
const char *game_get_player_rewrite_prompt(const game_state_t *game, int player_id);
const char *game_get_player_rewrite_submission(const game_state_t *game, int player_id);
bool game_pick_round_winner(const game_state_t *game,
                            char *username_out,
                            size_t username_out_size);
const char *game_action_result_message(game_action_result_t result);

#endif
