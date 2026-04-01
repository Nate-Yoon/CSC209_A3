/*
 * game.c
 *
 * Purpose:
 * Authoritative game-state module for the CSC209 A3 project.
 * This file owns lobby/player bookkeeping plus high-level phase transitions,
 * leaving server.c responsible for networking and message transport.
 */

#include "game.h"

#include <stdio.h>
#include <time.h>
#include <string.h>

static const char *const GAME_PROMPT_BANK_PATH = "question_prompts.txt";

static game_player_t *game_find_player(game_state_t *game, int player_id);
static const game_player_t *game_find_player_const(const game_state_t *game,
                                                   int player_id);
static int game_find_player_index(const game_state_t *game, int player_id);
static int game_find_open_player_slot(const game_state_t *game);
static bool game_username_is_unique(const game_state_t *game, const char *username);
static void game_reset_player(game_player_t *player);
static round_category_t game_category_for_round_number(int round_number);
static int game_get_rewrite_target_index_for_player(const game_state_t *game, int player_id);
static int game_get_reveal_owner_index_at(const game_state_t *game, size_t reveal_index);
static bool game_apply_round_scores(game_state_t *game);

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

int game_count_connected_players(const game_state_t *game) {
    int count;
    size_t i;

    if (game == NULL) {
        return 0;
    }

    count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].connected) {
            count++;
        }
    }

    return count;
}

int game_count_replay_players(const game_state_t *game) {
    int count;
    size_t i;

    if (game == NULL) {
        return 0;
    }

    count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].connected && game->players[i].wants_replay) {
            count++;
        }
    }

    return count;
}

int game_count_possible_replay_players(const game_state_t *game) {
    int count;
    size_t i;

    if (game == NULL) {
        return 0;
    }

    count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!game->players[i].connected) {
            continue;
        }

        if (!game->players[i].replay_decided || game->players[i].wants_replay) {
            count++;
        }
    }

    return count;
}

bool game_all_connected_players_want_replay(const game_state_t *game) {
    size_t i;
    bool saw_connected_player;

    if (game == NULL) {
        return false;
    }

    saw_connected_player = false;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!game->players[i].connected) {
            continue;
        }

        saw_connected_player = true;
        if (!game->players[i].replay_decided || !game->players[i].wants_replay) {
            return false;
        }
    }

    return saw_connected_player;
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
    player->replay_decided = false;
    player->wants_replay = false;
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

    if (game == NULL ||
        submission == NULL ||
        !protocol_submission_is_valid(submission)) {
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

    return game_begin_round(game);
}

bool game_begin_round(game_state_t *game) {
    size_t i;
    int next_round_number;

    if (game == NULL) {
        return false;
    }

    if (game->phase != GAME_PHASE_LOBBY && game->phase != GAME_PHASE_RESULTS) {
        return false;
    }

    next_round_number = game->round_number + 1;
    if (!round_begin(&game->current_round,
                     next_round_number,
                     game_category_for_round_number(next_round_number))) {
        return false;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (game->players[i].joined && game->players[i].connected) {
            round_set_player_active(&game->current_round, i, true);
        }
    }

    if (!round_assign_prompts_from_file(&game->current_round, GAME_PROMPT_BANK_PATH)) {
        fprintf(stderr, "game: failed to initialize round prompts from %s\n",
                GAME_PROMPT_BANK_PATH);
        round_state_reset(&game->current_round);
        return false;
    }

    round_set_submission_deadline(&game->current_round, 0);
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
        round_set_submission_deadline(&game->current_round, 0);
        if (!round_assign_rewrite_targets(&game->current_round)) {
            fprintf(stderr,
                    "game: could not assign title-writing targets for round %d; continuing to results\n",
                    game->round_number);
            game->phase = GAME_PHASE_RESULTS;
            return true;
        }

        round_set_rewrite_deadline(&game->current_round, 0);
        game->phase = GAME_PHASE_REWRITE;
        return true;
    }

    if (game->phase == GAME_PHASE_REWRITE &&
        round_all_rewritten(&game->current_round)) {
        round_set_rewrite_deadline(&game->current_round, 0);
        if (!round_prepare_voting(&game->current_round)) {
            fprintf(stderr,
                    "game: could not prepare voting for round %d; continuing to results\n",
                    game->round_number);
            game->phase = GAME_PHASE_RESULTS;
            return true;
        }

        round_set_voting_deadline(&game->current_round, 0);
        game->phase = GAME_PHASE_VOTING;
        return true;
    }

    if (game->phase == GAME_PHASE_VOTING &&
        round_all_voted(&game->current_round)) {
        round_set_voting_deadline(&game->current_round, 0);
        if (!game_apply_round_scores(game)) {
            fprintf(stderr,
                    "game: could not finalize round %d scoring\n",
                    game->round_number);
        }
        game->phase = GAME_PHASE_RESULTS;
        return true;
    }

    return false;
}

bool game_finish_round(game_state_t *game) {
    if (game == NULL || game->phase != GAME_PHASE_RESULTS) {
        return false;
    }

    if (!game_is_final_round(game)) {
        return game_begin_round(game);
    }

    game_end(game);
    return true;
}

void game_end(game_state_t *game) {
    if (game == NULL) {
        return;
    }

    game->phase = GAME_PHASE_OVER;
}

game_action_result_t game_handle_replay_choice(game_state_t *game,
                                               int player_id,
                                               bool wants_replay) {
    game_player_t *player;

    if (game == NULL || player_id <= 0) {
        return GAME_ACTION_INVALID_INPUT;
    }

    if (game->phase != GAME_PHASE_OVER) {
        return GAME_ACTION_INVALID_STATE;
    }

    player = game_find_player(game, player_id);
    if (player == NULL || !player->connected) {
        return GAME_ACTION_UNKNOWN_PLAYER;
    }

    player->replay_decided = true;
    player->wants_replay = wants_replay;
    return GAME_ACTION_OK;
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
    player->replay_decided = false;
    player->wants_replay = false;
    if (game->current_round.active) {
        round_set_player_active(&game->current_round, (size_t)player_index, false);
    }

    if (game->phase != GAME_PHASE_OVER &&
        game_count_joined_players(game) < PROTOCOL_MIN_PLAYERS) {
        game_end(game);
    }
}

void game_prepare_replay(game_state_t *game) {
    size_t i;

    if (game == NULL) {
        return;
    }

    game->phase = GAME_PHASE_LOBBY;
    game->round_number = 0;
    round_state_reset(&game->current_round);

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        game_player_t *player = &game->players[i];

        player->score = 0;
        player->joined = player->connected && player->wants_replay;
        player->ready = player->joined;
        player->replay_decided = false;
        player->wants_replay = false;
    }
}

void game_start_prompt_window(game_state_t *game, time_t now) {
    if (game == NULL || game->phase != GAME_PHASE_PROMPT) {
        return;
    }

    round_set_submission_deadline(&game->current_round,
                                  now + PROTOCOL_SUBMISSION_TIMEOUT_SECONDS);
}

void game_start_title_window(game_state_t *game, time_t now) {
    if (game == NULL || game->phase != GAME_PHASE_REWRITE) {
        return;
    }

    round_set_rewrite_deadline(&game->current_round,
                               now + PROTOCOL_TITLE_TIMEOUT_SECONDS);
}

void game_start_vote_window(game_state_t *game, time_t now) {
    if (game == NULL || game->phase != GAME_PHASE_VOTING) {
        return;
    }

    round_set_voting_deadline(&game->current_round,
                              now + PROTOCOL_VOTE_TIMEOUT_SECONDS);
}

time_t game_get_phase_deadline(const game_state_t *game) {
    if (game == NULL) {
        return 0;
    }

    if (game->phase == GAME_PHASE_PROMPT) {
        return round_get_submission_deadline(&game->current_round);
    }

    if (game->phase == GAME_PHASE_REWRITE) {
        return round_get_rewrite_deadline(&game->current_round);
    }

    if (game->phase == GAME_PHASE_VOTING) {
        return round_get_voting_deadline(&game->current_round);
    }

    return 0;
}

int game_apply_phase_timeout(game_state_t *game, time_t now) {
    time_t deadline;
    int applied_count;

    if (game == NULL) {
        return 0;
    }

    if (game->phase == GAME_PHASE_PROMPT) {
        deadline = round_get_submission_deadline(&game->current_round);
        if (deadline == 0 || now < deadline) {
            return 0;
        }

        round_set_submission_deadline(&game->current_round, 0);
        applied_count = round_apply_missing_fallbacks(&game->current_round);
        game_advance_phase_if_ready(game);
        return applied_count;
    }

    if (game->phase == GAME_PHASE_REWRITE) {
        deadline = round_get_rewrite_deadline(&game->current_round);
        if (deadline == 0 || now < deadline) {
            return 0;
        }

        round_set_rewrite_deadline(&game->current_round, 0);
        applied_count = round_apply_missing_rewrites(&game->current_round);
        game_advance_phase_if_ready(game);
        return applied_count;
    }

    if (game->phase == GAME_PHASE_VOTING) {
        deadline = round_get_voting_deadline(&game->current_round);
        if (deadline == 0 || now < deadline) {
            return 0;
        }

        round_set_voting_deadline(&game->current_round, 0);
        applied_count = round_apply_missing_votes(&game->current_round);
        game_advance_phase_if_ready(game);
        return applied_count;
    }

    return 0;
}

bool game_is_final_round(const game_state_t *game) {
    return game != NULL && game->round_number >= GAME_TOTAL_ROUNDS;
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

const char *game_get_player_prompt(const game_state_t *game, int player_id) {
    int player_index;

    if (game == NULL) {
        return NULL;
    }

    player_index = game_find_player_index(game, player_id);
    if (player_index < 0) {
        return NULL;
    }

    return round_get_player_prompt(&game->current_round, (size_t)player_index);
}

const char *game_get_player_rewrite_prompt(const game_state_t *game, int player_id) {
    int target_index;

    if (game == NULL) {
        return NULL;
    }

    target_index = game_get_rewrite_target_index_for_player(game, player_id);
    if (target_index < 0) {
        return NULL;
    }

    return round_get_player_prompt(&game->current_round, (size_t)target_index);
}

const char *game_get_player_rewrite_submission(const game_state_t *game, int player_id) {
    int target_index;

    if (game == NULL) {
        return NULL;
    }

    target_index = game_get_rewrite_target_index_for_player(game, player_id);
    if (target_index < 0) {
        return NULL;
    }

    return round_get_player_submission(&game->current_round, (size_t)target_index);
}

int game_get_reveal_entry_count(const game_state_t *game) {
    if (game == NULL) {
        return 0;
    }

    return round_get_reveal_count(&game->current_round);
}

const char *game_get_reveal_prompt_at(const game_state_t *game, size_t reveal_index) {
    int owner_index;

    owner_index = game_get_reveal_owner_index_at(game, reveal_index);
    if (owner_index < 0) {
        return NULL;
    }

    return round_get_player_prompt(&game->current_round, (size_t)owner_index);
}

const char *game_get_reveal_submission_at(const game_state_t *game, size_t reveal_index) {
    int owner_index;

    owner_index = game_get_reveal_owner_index_at(game, reveal_index);
    if (owner_index < 0) {
        return NULL;
    }

    return round_get_player_submission(&game->current_round, (size_t)owner_index);
}

const char *game_get_reveal_title_at(const game_state_t *game, size_t reveal_index) {
    int owner_index;

    owner_index = game_get_reveal_owner_index_at(game, reveal_index);
    if (owner_index < 0) {
        return NULL;
    }

    return round_get_title_for_submission_owner(&game->current_round, (size_t)owner_index);
}

int game_get_vote_target_player_id_at(const game_state_t *game, int option_number) {
    int owner_index;

    if (game == NULL || option_number <= 0) {
        return 0;
    }

    owner_index = game_get_reveal_owner_index_at(game, (size_t)(option_number - 1));
    if (owner_index < 0) {
        return 0;
    }

    return game->players[owner_index].player_id;
}

int game_get_player_forbidden_vote_option(const game_state_t *game, int player_id) {
    int player_index;
    int forbidden_owner_index;
    int reveal_index;

    if (game == NULL || player_id <= 0) {
        return 0;
    }

    player_index = game_find_player_index(game, player_id);
    if (player_index < 0) {
        return 0;
    }

    forbidden_owner_index = round_get_rewrite_target_index(&game->current_round,
                                                           (size_t)player_index);
    if (forbidden_owner_index < 0) {
        return 0;
    }

    for (reveal_index = 0;
         reveal_index < round_get_reveal_count(&game->current_round);
         reveal_index++) {
        if (round_get_reveal_owner_at(&game->current_round, (size_t)reveal_index) ==
            forbidden_owner_index) {
            return reveal_index + 1;
        }
    }

    return 0;
}

round_category_t game_get_round_category(const game_state_t *game) {
    const round_state_t *round;

    round = game_get_current_round(game);
    if (round == NULL) {
        return ROUND_CATEGORY_NONE;
    }

    return round->category;
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

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        if (!game->players[i].joined) {
            return i;
        }
    }

    return -1;
}

static bool game_username_is_unique(const game_state_t *game, const char *username) {
    size_t i;

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
    player->player_id = 0;
    player->username[0] = '\0';
    player->score = 0;
    player->connected = false;
    player->joined = false;
    player->ready = false;
    player->replay_decided = false;
    player->wants_replay = false;
}

static round_category_t game_category_for_round_number(int round_number) {
    switch (round_number) {
        case 1:
            return ROUND_CATEGORY_HEADLINES;
        case 2:
            return ROUND_CATEGORY_CAPTIONS;
        case 3:
            return ROUND_CATEGORY_REVIEWS;
        case 4:
            return ROUND_CATEGORY_FORUMS;
        default:
            return ROUND_CATEGORY_NONE;
    }
}

static int game_get_rewrite_target_index_for_player(const game_state_t *game, int player_id) {
    int player_index;

    if (game == NULL) {
        return ROUND_NO_TARGET;
    }

    player_index = game_find_player_index(game, player_id);
    if (player_index < 0) {
        return ROUND_NO_TARGET;
    }

    return round_get_rewrite_target_index(&game->current_round, (size_t)player_index);
}

static int game_get_reveal_owner_index_at(const game_state_t *game, size_t reveal_index) {
    if (game == NULL) {
        return ROUND_NO_ENTRY;
    }

    return round_get_reveal_owner_at(&game->current_round, reveal_index);
}

static bool game_apply_round_scores(game_state_t *game) {
    int reveal_count;
    int reveal_index;

    if (game == NULL) {
        return false;
    }

    reveal_count = round_get_reveal_count(&game->current_round);
    if (reveal_count <= 0) {
        return false;
    }

    for (reveal_index = 0; reveal_index < reveal_count; reveal_index++) {
        int owner_index;
        int title_writer_index;
        int vote_total;

        owner_index = round_get_reveal_owner_at(&game->current_round,
                                                (size_t)reveal_index);
        if (owner_index < 0) {
            continue;
        }

        vote_total = round_get_vote_total_for_submission_owner(&game->current_round,
                                                               (size_t)owner_index);
        if (vote_total <= 0) {
            continue;
        }

        title_writer_index =
            round_get_title_writer_for_submission_owner(&game->current_round,
                                                        (size_t)owner_index);
        if (title_writer_index >= 0) {
            game->players[title_writer_index].score +=
                vote_total * GAME_FIRST_TITLE_POINTS;
        }

        game->players[owner_index].score += vote_total * GAME_FIRST_ANSWER_POINTS;
    }

    return true;
}
