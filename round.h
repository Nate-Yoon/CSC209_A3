#ifndef ROUND_H
#define ROUND_H

/*
 * round.h
 *
 * Purpose:
 * Authoritative per-round gameplay state for the CSC209 A3 server.
 * This module owns prompt text plus per-player submission, rewrite/twist,
 * and voting storage so later gameplay phases can build on a stable shape.
 */

#include <stdbool.h>
#include <stddef.h>

#include "protocol.h"

enum {
    ROUND_MAX_PROMPT_LEN = PROTOCOL_MAX_SUBMISSION_LEN,
    ROUND_NO_VOTE = -1
};

typedef struct {
    bool active;
    bool has_submitted;
    bool has_rewritten;
    bool has_voted;
    char submission[PROTOCOL_MAX_SUBMISSION_LEN + 1];
    char rewrite_text[PROTOCOL_MAX_SUBMISSION_LEN + 1];
    int voted_for_index;
} round_player_state_t;

typedef struct {
    bool active;
    int round_number;
    int participant_count;
    int submission_count;
    int rewrite_count;
    int vote_count;
    char prompt[ROUND_MAX_PROMPT_LEN + 1];
    int vote_totals[PROTOCOL_MAX_PLAYERS];
    round_player_state_t players[PROTOCOL_MAX_PLAYERS];
} round_state_t;

void round_state_init(round_state_t *round);
void round_state_reset(round_state_t *round);
bool round_begin(round_state_t *round, int round_number, const char *prompt);
bool round_set_player_active(round_state_t *round, size_t player_index, bool active);
bool round_record_submission(round_state_t *round,
                             size_t player_index,
                             const char *submission);
bool round_record_rewrite(round_state_t *round,
                          size_t player_index,
                          const char *rewrite_text);
bool round_record_vote(round_state_t *round,
                       size_t voter_index,
                       size_t target_index);
bool round_all_submitted(const round_state_t *round);
bool round_all_rewritten(const round_state_t *round);
bool round_all_voted(const round_state_t *round);
int round_pick_random_submitted_index(const round_state_t *round);

#endif
