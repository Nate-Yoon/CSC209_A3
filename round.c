/*
 * round.c
 *
 * Purpose:
 * Per-round state and bookkeeping for the CSC209 A3 server.
 * This module now owns prompt-bank loading, per-player prompt assignment,
 * fallback submissions, and phase readiness checks for a single round.
 */

#include "round.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ROUND_FILE_BUFFER_SIZE = PROTOCOL_MAX_LINE_LEN + 2
};

typedef enum {
    ROUND_CONTENT_OK = 0,
    ROUND_CONTENT_EOF,
    ROUND_CONTENT_IO_ERROR,
    ROUND_CONTENT_LINE_TOO_LONG
} round_content_result_t;

typedef struct {
    char prompt[PROTOCOL_MAX_PROMPT_LEN + 1];
    char fallback[PROTOCOL_MAX_SUBMISSION_LEN + 1];
} round_prompt_pair_t;

static void round_reset_player_state(round_player_state_t *player_state);
static round_category_t round_category_from_header(const char *line);
static bool round_load_prompt_pairs(const char *file_path,
                                    round_category_t category,
                                    round_prompt_pair_t **pairs_out,
                                    size_t *pair_count_out);
static bool round_prompt_bank_text_is_valid(const char *text);
static round_content_result_t round_read_next_content_line(FILE *file,
                                                           char *buffer,
                                                           size_t buffer_size,
                                                           size_t *line_number);
static bool round_report_load_error(const char *file_path,
                                    size_t line_number,
                                    const char *reason);
static void round_strip_newline(char *text);

void round_state_init(round_state_t *round) {
    if (round == NULL) {
        return;
    }

    round_state_reset(round);
}

void round_state_reset(round_state_t *round) {
    size_t i;

    if (round == NULL) {
        return;
    }

    round->active = false;
    round->round_number = 0;
    round->category = ROUND_CATEGORY_NONE;
    round->participant_count = 0;
    round->submission_count = 0;
    round->rewrite_count = 0;
    round->vote_count = 0;
    round->reveal_count = 0;
    round->submission_deadline = 0;
    round->rewrite_deadline = 0;
    round->voting_deadline = 0;
    round->winning_entry_index = ROUND_NO_ENTRY;
    round->winning_title_writer_index = ROUND_NO_TARGET;
    round->winning_vote_total = 0;
    round->runner_up_entry_index = ROUND_NO_ENTRY;
    round->runner_up_title_writer_index = ROUND_NO_TARGET;
    round->runner_up_vote_total = 0;

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        round->reveal_order[i] = ROUND_NO_ENTRY;
        round->vote_totals[i] = 0;
        round_reset_player_state(&round->players[i]);
    }
}

bool round_begin(round_state_t *round,
                 int round_number,
                 round_category_t category) {
    if (round == NULL || round_number <= 0 || category == ROUND_CATEGORY_NONE) {
        return false;
    }

    round_state_reset(round);
    round->active = true;
    round->round_number = round_number;
    round->category = category;
    return true;
}

bool round_set_player_active(round_state_t *round, size_t player_index, bool active) {
    round_player_state_t *player_state;

    if (round == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return false;
    }

    player_state = &round->players[player_index];
    if (player_state->active == active) {
        return true;
    }

    if (active) {
        player_state->active = true;
        round->participant_count++;
        return true;
    }

    player_state->active = false;
    player_state->rewrite_target_index = ROUND_NO_TARGET;
    if (round->participant_count > 0) {
        round->participant_count--;
    }
    return true;
}

bool round_assign_prompts_from_file(round_state_t *round, const char *file_path) {
    round_prompt_pair_t *pairs;
    int *indices;
    size_t pair_count;
    size_t chosen_count;
    size_t i;

    if (round == NULL || file_path == NULL || !round->active || round->participant_count <= 0) {
        return false;
    }

    pairs = NULL;
    pair_count = 0;
    if (!round_load_prompt_pairs(file_path, round->category, &pairs, &pair_count)) {
        return false;
    }

    if (pair_count < (size_t)round->participant_count) {
        fprintf(stderr,
                "round: prompt bank %s has only %zu usable prompt/default pairs, but %d players are active\n",
                file_path,
                pair_count,
                round->participant_count);
        free(pairs);
        return false;
    }

    indices = malloc(pair_count * sizeof(*indices));
    if (indices == NULL) {
        fprintf(stderr, "round: failed to allocate prompt assignment indices\n");
        free(pairs);
        return false;
    }

    for (i = 0; i < pair_count; i++) {
        indices[i] = (int)i;
    }

    for (i = pair_count; i > 1; i--) {
        size_t swap_index = (size_t)(rand() % (int)i);
        int temp = indices[i - 1];
        indices[i - 1] = indices[swap_index];
        indices[swap_index] = temp;
    }

    chosen_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        round_player_state_t *player_state = &round->players[i];
        const round_prompt_pair_t *pair;

        if (!player_state->active) {
            continue;
        }

        pair = &pairs[indices[chosen_count]];
        chosen_count++;

        strncpy(player_state->assigned_prompt,
                pair->prompt,
                sizeof(player_state->assigned_prompt) - 1);
        player_state->assigned_prompt[sizeof(player_state->assigned_prompt) - 1] = '\0';
        strncpy(player_state->fallback_submission,
                pair->fallback,
                sizeof(player_state->fallback_submission) - 1);
        player_state->fallback_submission[sizeof(player_state->fallback_submission) - 1] = '\0';
    }

    free(indices);
    free(pairs);
    return true;
}

bool round_assign_rewrite_targets(round_state_t *round) {
    int active_indices[PROTOCOL_MAX_PLAYERS];
    int active_count;
    int i;

    if (round == NULL || !round->active || round->participant_count < 2) {
        return false;
    }

    active_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!round->players[i].active || !round->players[i].has_submitted) {
            continue;
        }

        round->players[i].rewrite_target_index = ROUND_NO_TARGET;
        round->players[i].has_rewritten = false;
        round->players[i].rewrite_text[0] = '\0';
        active_indices[active_count] = i;
        active_count++;
    }

    if (active_count < 2) {
        return false;
    }

    round->rewrite_count = 0;
    for (i = active_count; i > 1; i--) {
        int swap_index = rand() % i;
        int temp = active_indices[i - 1];
        active_indices[i - 1] = active_indices[swap_index];
        active_indices[swap_index] = temp;
    }

    for (i = 0; i < active_count; i++) {
        int player_index = active_indices[i];
        int target_index = active_indices[(i + 1) % active_count];

        round->players[player_index].rewrite_target_index = target_index;
    }

    return true;
}

bool round_record_submission(round_state_t *round,
                             size_t player_index,
                             const char *submission) {
    round_player_state_t *player_state;

    if (round == NULL || submission == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return false;
    }

    player_state = &round->players[player_index];
    if (!round->active || !player_state->active || player_state->has_submitted) {
        return false;
    }

    strncpy(player_state->submission, submission, sizeof(player_state->submission) - 1);
    player_state->submission[sizeof(player_state->submission) - 1] = '\0';
    player_state->has_submitted = true;
    player_state->submitted_manually = true;
    player_state->used_fallback = false;
    round->submission_count++;
    return true;
}

bool round_record_rewrite(round_state_t *round,
                          size_t player_index,
                          const char *rewrite_text) {
    round_player_state_t *player_state;

    if (round == NULL || rewrite_text == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return false;
    }

    player_state = &round->players[player_index];
    if (!round->active ||
        !player_state->active ||
        player_state->has_rewritten ||
        player_state->rewrite_target_index == ROUND_NO_TARGET) {
        return false;
    }

    strncpy(player_state->rewrite_text,
            rewrite_text,
            sizeof(player_state->rewrite_text) - 1);
    player_state->rewrite_text[sizeof(player_state->rewrite_text) - 1] = '\0';
    player_state->has_rewritten = true;
    round->rewrite_count++;
    return true;
}

bool round_apply_fallback_submission(round_state_t *round, size_t player_index) {
    round_player_state_t *player_state;

    if (round == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return false;
    }

    player_state = &round->players[player_index];
    if (!round->active ||
        !player_state->active ||
        player_state->has_submitted ||
        player_state->fallback_submission[0] == '\0') {
        return false;
    }

    strncpy(player_state->submission,
            player_state->fallback_submission,
            sizeof(player_state->submission) - 1);
    player_state->submission[sizeof(player_state->submission) - 1] = '\0';
    player_state->has_submitted = true;
    player_state->submitted_manually = false;
    player_state->used_fallback = true;
    round->submission_count++;
    return true;
}

int round_apply_missing_fallbacks(round_state_t *round) {
    int applied_count;
    size_t i;

    if (round == NULL || !round->active) {
        return 0;
    }

    applied_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (round_apply_fallback_submission(round, i)) {
            applied_count++;
        }
    }

    return applied_count;
}

bool round_apply_empty_rewrite(round_state_t *round, size_t player_index) {
    round_player_state_t *player_state;

    if (round == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return false;
    }

    player_state = &round->players[player_index];
    if (!round->active ||
        !player_state->active ||
        player_state->has_rewritten ||
        player_state->rewrite_target_index == ROUND_NO_TARGET) {
        return false;
    }

    player_state->rewrite_text[0] = '\0';
    player_state->has_rewritten = true;
    round->rewrite_count++;
    return true;
}

int round_apply_missing_rewrites(round_state_t *round) {
    int applied_count;
    size_t i;

    if (round == NULL || !round->active) {
        return 0;
    }

    applied_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (round_apply_empty_rewrite(round, i)) {
            applied_count++;
        }
    }

    return applied_count;
}

bool round_prepare_voting(round_state_t *round) {
    int active_indices[PROTOCOL_MAX_PLAYERS];
    int active_count;
    int i;

    if (round == NULL || !round->active || round->participant_count <= 0) {
        return false;
    }

    active_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!round->players[i].active || !round->players[i].has_submitted) {
            continue;
        }

        active_indices[active_count] = i;
        active_count++;
    }

    if (active_count == 0) {
        return false;
    }

    for (i = active_count; i > 1; i--) {
        int swap_index = rand() % i;
        int temp = active_indices[i - 1];
        active_indices[i - 1] = active_indices[swap_index];
        active_indices[swap_index] = temp;
    }

    round->reveal_count = active_count;
    round->vote_count = 0;
    round->winning_entry_index = ROUND_NO_ENTRY;
    round->winning_title_writer_index = ROUND_NO_TARGET;
    round->winning_vote_total = 0;
    round->runner_up_entry_index = ROUND_NO_ENTRY;
    round->runner_up_title_writer_index = ROUND_NO_TARGET;
    round->runner_up_vote_total = 0;

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        round->reveal_order[i] = ROUND_NO_ENTRY;
        round->vote_totals[i] = 0;
        round->players[i].has_voted = false;
        round->players[i].voted_for_index = ROUND_NO_VOTE;
    }

    for (i = 0; i < active_count; i++) {
        round->reveal_order[i] = active_indices[i];
    }

    return true;
}

bool round_record_vote(round_state_t *round,
                       size_t voter_index,
                       size_t target_index) {
    round_player_state_t *voter_state;

    if (round == NULL ||
        voter_index >= PROTOCOL_MAX_PLAYERS ||
        target_index >= PROTOCOL_MAX_PLAYERS) {
        return false;
    }

    voter_state = &round->players[voter_index];
    if (!round->active ||
        !voter_state->active ||
        !round->players[target_index].active ||
        voter_state->has_voted ||
        voter_state->rewrite_target_index == (int)target_index) {
        return false;
    }

    voter_state->has_voted = true;
    voter_state->voted_for_index = (int)target_index;
    round->vote_totals[target_index]++;
    round->vote_count++;
    return true;
}

bool round_apply_missing_vote(round_state_t *round, size_t voter_index) {
    round_player_state_t *voter_state;

    if (round == NULL || voter_index >= PROTOCOL_MAX_PLAYERS) {
        return false;
    }

    voter_state = &round->players[voter_index];
    if (!round->active || !voter_state->active || voter_state->has_voted) {
        return false;
    }

    voter_state->has_voted = true;
    voter_state->voted_for_index = ROUND_NO_VOTE;
    return true;
}

int round_apply_missing_votes(round_state_t *round) {
    int applied_count;
    size_t i;

    if (round == NULL || !round->active) {
        return 0;
    }

    applied_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (round_apply_missing_vote(round, i)) {
            applied_count++;
        }
    }

    return applied_count;
}

void round_set_submission_deadline(round_state_t *round, time_t deadline) {
    if (round == NULL) {
        return;
    }

    round->submission_deadline = deadline;
}

time_t round_get_submission_deadline(const round_state_t *round) {
    if (round == NULL) {
        return 0;
    }

    return round->submission_deadline;
}

void round_set_rewrite_deadline(round_state_t *round, time_t deadline) {
    if (round == NULL) {
        return;
    }

    round->rewrite_deadline = deadline;
}

time_t round_get_rewrite_deadline(const round_state_t *round) {
    if (round == NULL) {
        return 0;
    }

    return round->rewrite_deadline;
}

void round_set_voting_deadline(round_state_t *round, time_t deadline) {
    if (round == NULL) {
        return;
    }

    round->voting_deadline = deadline;
}

time_t round_get_voting_deadline(const round_state_t *round) {
    if (round == NULL) {
        return 0;
    }

    return round->voting_deadline;
}

int round_get_rewrite_target_index(const round_state_t *round, size_t player_index) {
    if (round == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return ROUND_NO_TARGET;
    }

    return round->players[player_index].rewrite_target_index;
}

int round_get_reveal_count(const round_state_t *round) {
    if (round == NULL) {
        return 0;
    }

    return round->reveal_count;
}

int round_get_reveal_owner_at(const round_state_t *round, size_t reveal_index) {
    if (round == NULL || reveal_index >= PROTOCOL_MAX_PLAYERS) {
        return ROUND_NO_ENTRY;
    }

    return round->reveal_order[reveal_index];
}

const char *round_get_player_prompt(const round_state_t *round, size_t player_index) {
    if (round == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return NULL;
    }

    return round->players[player_index].assigned_prompt;
}

const char *round_get_player_submission(const round_state_t *round, size_t player_index) {
    if (round == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return NULL;
    }

    return round->players[player_index].submission;
}

int round_get_title_writer_for_submission_owner(const round_state_t *round,
                                                size_t player_index) {
    size_t i;

    if (round == NULL || player_index >= PROTOCOL_MAX_PLAYERS) {
        return ROUND_NO_TARGET;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!round->players[i].active) {
            continue;
        }

        if (round->players[i].rewrite_target_index == (int)player_index) {
            return (int)i;
        }
    }

    return ROUND_NO_TARGET;
}

const char *round_get_title_for_submission_owner(const round_state_t *round,
                                                 size_t player_index) {
    int title_writer_index;

    title_writer_index = round_get_title_writer_for_submission_owner(round, player_index);
    if (title_writer_index < 0) {
        return NULL;
    }

    return round->players[title_writer_index].rewrite_text;
}

bool round_finalize_winner(round_state_t *round) {
    int best_targets[PROTOCOL_MAX_PLAYERS];
    int runner_up_targets[PROTOCOL_MAX_PLAYERS];
    int best_count;
    int best_vote_total;
    int runner_up_count;
    int runner_up_vote_total;
    int i;

    if (round == NULL || !round->active || round->reveal_count <= 0) {
        return false;
    }

    best_count = 0;
    best_vote_total = -1;
    runner_up_count = 0;
    runner_up_vote_total = -1;
    for (i = 0; i < round->reveal_count; i++) {
        int owner_index = round->reveal_order[i];
        int owner_votes;

        if (owner_index < 0) {
            continue;
        }

        owner_votes = round->vote_totals[owner_index];
        if (owner_votes > best_vote_total) {
            best_vote_total = owner_votes;
            best_targets[0] = owner_index;
            best_count = 1;
        } else if (owner_votes == best_vote_total) {
            best_targets[best_count] = owner_index;
            best_count++;
        }
    }

    if (best_count <= 0) {
        return false;
    }

    round->winning_entry_index = best_targets[rand() % best_count];
    round->winning_title_writer_index =
        round_get_title_writer_for_submission_owner(round,
                                                    (size_t)round->winning_entry_index);
    round->winning_vote_total = best_vote_total;

    for (i = 0; i < round->reveal_count; i++) {
        int owner_index = round->reveal_order[i];
        int owner_votes;

        if (owner_index < 0 || owner_index == round->winning_entry_index) {
            continue;
        }

        owner_votes = round->vote_totals[owner_index];
        if (owner_votes > runner_up_vote_total) {
            runner_up_vote_total = owner_votes;
            runner_up_targets[0] = owner_index;
            runner_up_count = 1;
        } else if (owner_votes == runner_up_vote_total) {
            runner_up_targets[runner_up_count] = owner_index;
            runner_up_count++;
        }
    }

    if (runner_up_count > 0) {
        round->runner_up_entry_index = runner_up_targets[rand() % runner_up_count];
        round->runner_up_title_writer_index =
            round_get_title_writer_for_submission_owner(round,
                                                        (size_t)round->runner_up_entry_index);
        round->runner_up_vote_total = runner_up_vote_total;
    }

    return round->winning_title_writer_index >= 0;
}

int round_get_winning_entry_index(const round_state_t *round) {
    if (round == NULL) {
        return ROUND_NO_ENTRY;
    }

    return round->winning_entry_index;
}

int round_get_winning_title_writer_index(const round_state_t *round) {
    if (round == NULL) {
        return ROUND_NO_TARGET;
    }

    return round->winning_title_writer_index;
}

int round_get_winning_vote_total(const round_state_t *round) {
    if (round == NULL) {
        return 0;
    }

    return round->winning_vote_total;
}

int round_get_runner_up_entry_index(const round_state_t *round) {
    if (round == NULL) {
        return ROUND_NO_ENTRY;
    }

    return round->runner_up_entry_index;
}

int round_get_runner_up_title_writer_index(const round_state_t *round) {
    if (round == NULL) {
        return ROUND_NO_TARGET;
    }

    return round->runner_up_title_writer_index;
}

int round_get_runner_up_vote_total(const round_state_t *round) {
    if (round == NULL) {
        return 0;
    }

    return round->runner_up_vote_total;
}

bool round_all_submitted(const round_state_t *round) {
    size_t i;

    if (round == NULL || !round->active || round->participant_count == 0) {
        return false;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!round->players[i].active) {
            continue;
        }

        if (!round->players[i].has_submitted) {
            return false;
        }
    }

    return true;
}

bool round_all_rewritten(const round_state_t *round) {
    size_t i;

    if (round == NULL || !round->active || round->participant_count == 0) {
        return false;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!round->players[i].active) {
            continue;
        }

        if (!round->players[i].has_rewritten) {
            return false;
        }
    }

    return true;
}

bool round_all_voted(const round_state_t *round) {
    size_t i;

    if (round == NULL || !round->active || round->participant_count == 0) {
        return false;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!round->players[i].active) {
            continue;
        }

        if (!round->players[i].has_voted) {
            return false;
        }
    }

    return true;
}

int round_pick_random_submitted_index(const round_state_t *round) {
    int eligible_indices[PROTOCOL_MAX_PLAYERS];
    int eligible_count;
    size_t i;

    if (round == NULL || !round->active) {
        return -1;
    }

    eligible_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!round->players[i].has_submitted) {
            continue;
        }

        eligible_indices[eligible_count] = (int)i;
        eligible_count++;
    }

    if (eligible_count == 0) {
        return -1;
    }

    return eligible_indices[rand() % eligible_count];
}

static void round_reset_player_state(round_player_state_t *player_state) {
    player_state->active = false;
    player_state->has_submitted = false;
    player_state->submitted_manually = false;
    player_state->used_fallback = false;
    player_state->has_rewritten = false;
    player_state->has_voted = false;
    player_state->rewrite_target_index = ROUND_NO_TARGET;
    player_state->assigned_prompt[0] = '\0';
    player_state->fallback_submission[0] = '\0';
    player_state->submission[0] = '\0';
    player_state->rewrite_text[0] = '\0';
    player_state->voted_for_index = ROUND_NO_VOTE;
}

static round_category_t round_category_from_header(const char *line) {
    if (line[0] != '#') {
        return ROUND_CATEGORY_NONE;
    }

    if (strcmp(line, "#headlines") == 0) {
        return ROUND_CATEGORY_HEADLINES;
    }
    if (strcmp(line, "#captions") == 0) {
        return ROUND_CATEGORY_CAPTIONS;
    }
    if (strcmp(line, "#reviews") == 0) {
        return ROUND_CATEGORY_REVIEWS;
    }
    if (strcmp(line, "#forums") == 0) {
        return ROUND_CATEGORY_FORUMS;
    }

    return ROUND_CATEGORY_NONE;
}

static bool round_load_prompt_pairs(const char *file_path,
                                    round_category_t category,
                                    round_prompt_pair_t **pairs_out,
                                    size_t *pair_count_out) {
    FILE *file;
    round_prompt_pair_t *pairs;
    size_t pair_count;
    size_t capacity;
    size_t line_number;
    round_category_t current_category;
    char prompt_buffer[ROUND_FILE_BUFFER_SIZE];
    char fallback_buffer[ROUND_FILE_BUFFER_SIZE];

    if (file_path == NULL ||
        pairs_out == NULL ||
        pair_count_out == NULL ||
        category == ROUND_CATEGORY_NONE) {
        return false;
    }

    file = fopen(file_path, "r");
    if (file == NULL) {
        fprintf(stderr, "round: could not open prompt bank %s: %s\n",
                file_path, strerror(errno));
        return false;
    }

    pairs = NULL;
    pair_count = 0;
    capacity = 0;
    line_number = 0;
    current_category = ROUND_CATEGORY_NONE;

    for (;;) {
        round_content_result_t prompt_result;
        round_content_result_t fallback_result;
        round_prompt_pair_t *resized_pairs;
        round_category_t header_category;

        prompt_result = round_read_next_content_line(file,
                                                     prompt_buffer,
                                                     sizeof(prompt_buffer),
                                                     &line_number);
        if (prompt_result == ROUND_CONTENT_EOF) {
            break;
        }
        if (prompt_result == ROUND_CONTENT_IO_ERROR) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "failed while reading prompt bank");
        }
        if (prompt_result == ROUND_CONTENT_LINE_TOO_LONG) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "line is too long or missing a newline before the line limit");
        }

        header_category = round_category_from_header(prompt_buffer);
        if (header_category != ROUND_CATEGORY_NONE) {
            current_category = header_category;
            continue;
        }

        if (prompt_buffer[0] == '#') {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "unknown category header");
        }

        if (current_category == ROUND_CATEGORY_NONE) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "found prompt text before any category header");
        }

        fallback_result = round_read_next_content_line(file,
                                                       fallback_buffer,
                                                       sizeof(fallback_buffer),
                                                       &line_number);
        if (fallback_result == ROUND_CONTENT_EOF) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "odd number of lines: prompt is missing its fallback answer");
        }
        if (fallback_result == ROUND_CONTENT_IO_ERROR) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "failed while reading fallback answer line");
        }
        if (fallback_result == ROUND_CONTENT_LINE_TOO_LONG) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "fallback answer line is too long or missing a newline before the line limit");
        }

        if (round_category_from_header(fallback_buffer) != ROUND_CATEGORY_NONE ||
            fallback_buffer[0] == '#') {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "prompt is missing its fallback answer before the next category header");
        }

        if (strlen(prompt_buffer) > PROTOCOL_MAX_PROMPT_LEN) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number - 1,
                                           "prompt exceeds the maximum allowed prompt length");
        }

        if (strlen(fallback_buffer) > PROTOCOL_MAX_SUBMISSION_LEN) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "fallback answer exceeds the maximum allowed submission length");
        }

        if (!round_prompt_bank_text_is_valid(fallback_buffer)) {
            free(pairs);
            fclose(file);
            return round_report_load_error(file_path,
                                           line_number,
                                           "fallback answer contains invalid submission characters");
        }

        if (current_category != category) {
            continue;
        }

        if (pair_count == capacity) {
            size_t new_capacity = (capacity == 0) ? 8 : capacity * 2;

            resized_pairs = realloc(pairs, new_capacity * sizeof(*pairs));
            if (resized_pairs == NULL) {
                free(pairs);
                fclose(file);
                fprintf(stderr, "round: failed to grow prompt pair storage\n");
                return false;
            }

            pairs = resized_pairs;
            capacity = new_capacity;
        }

        strncpy(pairs[pair_count].prompt, prompt_buffer, sizeof(pairs[pair_count].prompt) - 1);
        pairs[pair_count].prompt[sizeof(pairs[pair_count].prompt) - 1] = '\0';
        strncpy(pairs[pair_count].fallback,
                fallback_buffer,
                sizeof(pairs[pair_count].fallback) - 1);
        pairs[pair_count].fallback[sizeof(pairs[pair_count].fallback) - 1] = '\0';
        pair_count++;
    }

    if (ferror(file)) {
        free(pairs);
        fclose(file);
        fprintf(stderr, "round: failed while finishing prompt bank read for %s\n",
                file_path);
        return false;
    }

    if (pair_count == 0) {
        free(pairs);
        fclose(file);
        return round_report_load_error(file_path, 0,
                                       "no usable prompt/default pairs found for the requested category");
    }

    fclose(file);
    *pairs_out = pairs;
    *pair_count_out = pair_count;
    return true;
}

static bool round_prompt_bank_text_is_valid(const char *text) {
    size_t i;

    if (text == NULL || text[0] == '\0') {
        return false;
    }

    for (i = 0; text[i] != '\0'; i++) {
        unsigned char byte = (unsigned char)text[i];

        if (byte < 32 || byte == '|') {
            return false;
        }
    }

    return true;
}

static round_content_result_t round_read_next_content_line(FILE *file,
                                                           char *buffer,
                                                           size_t buffer_size,
                                                           size_t *line_number) {
    if (file == NULL || buffer == NULL || buffer_size == 0) {
        return ROUND_CONTENT_IO_ERROR;
    }

    for (;;) {
        if (fgets(buffer, (int)buffer_size, file) == NULL) {
            if (feof(file)) {
                return ROUND_CONTENT_EOF;
            }

            return ROUND_CONTENT_IO_ERROR;
        }

        if (line_number != NULL) {
            (*line_number)++;
        }

        if (strchr(buffer, '\n') == NULL && !feof(file)) {
            return ROUND_CONTENT_LINE_TOO_LONG;
        }

        round_strip_newline(buffer);
        if (buffer[0] == '\0') {
            continue;
        }

        return ROUND_CONTENT_OK;
    }
}

static bool round_report_load_error(const char *file_path,
                                    size_t line_number,
                                    const char *reason) {
    if (line_number == 0) {
        fprintf(stderr, "round: prompt bank %s is invalid: %s\n",
                file_path, reason);
        return false;
    }

    fprintf(stderr, "round: prompt bank %s is invalid near line %zu: %s\n",
            file_path, line_number, reason);
    return false;
}

static void round_strip_newline(char *text) {
    size_t len;

    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}
