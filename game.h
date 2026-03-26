#ifndef GAME_H
#define GAME_H

/*
 * game.h
 *
 * Purpose:
 * High-level game-state boundary for the CSC209 A3 server.
 * The server owns networking, while this module owns the authoritative
 * round/phase data used by gameplay.
 */

#include "protocol.h"

enum {
    GAME_MAX_PROMPT_LEN = PROTOCOL_MAX_SUBMISSION_LEN
};

typedef enum {
    GAME_PHASE_LOBBY = 0,
    GAME_PHASE_PROMPT,
    GAME_PHASE_RESULTS,
    GAME_PHASE_OVER
} game_phase_t;

typedef struct {
    int player_id;
    char username[PROTOCOL_MAX_USERNAME_LEN + 1];
    char submission[PROTOCOL_MAX_SUBMISSION_LEN + 1];
    bool active;
    bool has_submitted;
} game_player_t;

typedef struct {
    game_phase_t phase;
    int round_number;
    int player_count;
    int submitted_count;
    char current_prompt[GAME_MAX_PROMPT_LEN + 1];
    game_player_t players[PROTOCOL_MAX_PLAYERS];
} game_state_t;

void game_state_init(game_state_t *game);
void game_state_reset(game_state_t *game);
bool game_can_start(int player_count);
bool game_start(game_state_t *game,
                const int *player_ids,
                const char usernames[][PROTOCOL_MAX_USERNAME_LEN + 1],
                int player_count);
bool game_submit(game_state_t *game, int player_id, const char *submission);
bool game_all_submitted(const game_state_t *game);
const game_player_t *game_get_player(const game_state_t *game, int player_id);

#endif
