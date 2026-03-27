/*
 * round.c
 *
 * Purpose:
 * Per-round state and bookkeeping for the CSC209 A3 server.
 * This module intentionally stops short of implementing full gameplay rules;
 * it provides the storage and readiness checks that later phases will need.
 */

#include "round.h"

#include <stdlib.h>
#include <string.h>

static void round_reset_player_state(round_player_state_t *player_state);

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
    round->participant_count = 0;
    round->submission_count = 0;
    round->rewrite_count = 0;
    round->vote_count = 0;
    round->prompt[0] = '\0';

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        round->vote_totals[i] = 0;
        round_reset_player_state(&round->players[i]);
    }
}

bool round_begin(round_state_t *round, int round_number, const char *prompt) {
    if (round == NULL || prompt == NULL || round_number <= 0) {
        return false;
    }

    round_state_reset(round);
    round->active = true;
    round->round_number = round_number;
    strncpy(round->prompt, prompt, sizeof(round->prompt) - 1);
    round->prompt[sizeof(round->prompt) - 1] = '\0';
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
    if (round->participant_count > 0) {
        round->participant_count--;
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
    if (!round->active || !player_state->active || player_state->has_rewritten) {
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
        voter_index == target_index) {
        return false;
    }

    voter_state->has_voted = true;
    voter_state->voted_for_index = (int)target_index;
    round->vote_totals[target_index]++;
    round->vote_count++;
    return true;
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
    if (player_state == NULL) {
        return;
    }

    player_state->active = false;
    player_state->has_submitted = false;
    player_state->has_rewritten = false;
    player_state->has_voted = false;
    player_state->submission[0] = '\0';
    player_state->rewrite_text[0] = '\0';
    player_state->voted_for_index = ROUND_NO_VOTE;
}
