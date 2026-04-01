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
#include <time.h>

#include "protocol.h"

enum {
    ROUND_NO_VOTE = -1,
    ROUND_NO_TARGET = -1,
    ROUND_NO_ENTRY = -1
};

typedef enum {
    ROUND_CATEGORY_NONE = 0,
    ROUND_CATEGORY_HEADLINES,
    ROUND_CATEGORY_CAPTIONS,
    ROUND_CATEGORY_REVIEWS,
    ROUND_CATEGORY_FORUMS
} round_category_t;

typedef struct {
    bool active;
    bool has_submitted;
    bool submitted_manually;
    bool used_fallback;
    bool has_rewritten;
    bool has_voted;
    int rewrite_target_index;
    char assigned_prompt[PROTOCOL_MAX_PROMPT_LEN + 1];
    char fallback_submission[PROTOCOL_MAX_SUBMISSION_LEN + 1];
    char submission[PROTOCOL_MAX_SUBMISSION_LEN + 1];
    char rewrite_text[PROTOCOL_MAX_SUBMISSION_LEN + 1];
    int voted_for_index;
} round_player_state_t;

typedef struct {
    bool active;
    int round_number;
    round_category_t category;
    int participant_count;
    int submission_count;
    int rewrite_count;
    int vote_count;
    int reveal_count;
    time_t submission_deadline;
    time_t rewrite_deadline;
    time_t voting_deadline;
    int reveal_order[PROTOCOL_MAX_PLAYERS];
    int vote_totals[PROTOCOL_MAX_PLAYERS];
    round_player_state_t players[PROTOCOL_MAX_PLAYERS];
} round_state_t;

void round_state_init(round_state_t *round);
void round_state_reset(round_state_t *round);
bool round_begin(round_state_t *round,
                 int round_number,
                 round_category_t category);
bool round_set_player_active(round_state_t *round, size_t player_index, bool active);
bool round_assign_prompts_from_file(round_state_t *round, const char *file_path);
bool round_assign_rewrite_targets(round_state_t *round);
bool round_record_submission(round_state_t *round,
                             size_t player_index,
                             const char *submission);
bool round_record_rewrite(round_state_t *round,
                          size_t player_index,
                          const char *rewrite_text);
bool round_apply_fallback_submission(round_state_t *round, size_t player_index);
int round_apply_missing_fallbacks(round_state_t *round);
bool round_apply_empty_rewrite(round_state_t *round, size_t player_index);
int round_apply_missing_rewrites(round_state_t *round);
bool round_prepare_voting(round_state_t *round);
bool round_record_vote(round_state_t *round,
                       size_t voter_index,
                       size_t target_index);
bool round_apply_missing_vote(round_state_t *round, size_t voter_index);
int round_apply_missing_votes(round_state_t *round);
void round_set_submission_deadline(round_state_t *round, time_t deadline);
time_t round_get_submission_deadline(const round_state_t *round);
void round_set_rewrite_deadline(round_state_t *round, time_t deadline);
time_t round_get_rewrite_deadline(const round_state_t *round);
void round_set_voting_deadline(round_state_t *round, time_t deadline);
time_t round_get_voting_deadline(const round_state_t *round);
int round_get_rewrite_target_index(const round_state_t *round, size_t player_index);
int round_get_reveal_count(const round_state_t *round);
int round_get_reveal_owner_at(const round_state_t *round, size_t reveal_index);
int round_get_vote_total_for_submission_owner(const round_state_t *round,
                                              size_t player_index);
const char *round_get_player_prompt(const round_state_t *round, size_t player_index);
const char *round_get_player_submission(const round_state_t *round, size_t player_index);
int round_get_title_writer_for_submission_owner(const round_state_t *round,
                                                size_t player_index);
const char *round_get_title_for_submission_owner(const round_state_t *round,
                                                 size_t player_index);
bool round_all_submitted(const round_state_t *round);
bool round_all_rewritten(const round_state_t *round);
bool round_all_voted(const round_state_t *round);
int round_pick_random_submitted_index(const round_state_t *round);

#endif
