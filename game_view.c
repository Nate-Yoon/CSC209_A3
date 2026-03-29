#include "game_view.h"

#include <stdio.h>
#include <string.h>

enum {
    GAME_VIEW_STAGE_WIDTH = 40
};

static void game_view_broadcast(const game_view_sink_t *sink, const char *text);
static void game_view_send_player(const game_view_sink_t *sink,
                                  int player_id,
                                  const char *message);
static void game_view_pause(const game_view_sink_t *sink);
static void game_view_format_stage_line(char *buffer,
                                        size_t buffer_size,
                                        const char *label);
static const char *game_view_title_text(const char *title_text);
static const char *game_view_round_name(round_category_t category);
static const char *game_view_round_category_key(round_category_t category);
static const char *game_view_round_submission_label(round_category_t category);
static void game_view_broadcast_reveal_option(const game_view_sink_t *sink,
                                              round_category_t category,
                                              int option_number,
                                              const char *submission_text,
                                              const char *title_text);
static bool game_view_reveal_twist_first(round_category_t category);
static bool game_view_player_is_active(const game_player_t *player);

void game_view_broadcast_stage_banner(const game_view_sink_t *sink,
                                      const char *label) {
    static const char separator[] = "----------------------------------------";
    char label_line[PROTOCOL_LINE_BUFFER_SIZE];

    if (label == NULL) {
        return;
    }

    game_view_broadcast(sink, "");
    game_view_broadcast(sink, separator);
    game_view_format_stage_line(label_line, sizeof(label_line), label);
    game_view_broadcast(sink, label_line);
    game_view_broadcast(sink, separator);
}

void game_view_broadcast_round_intro(const game_state_t *game,
                                     const game_view_sink_t *sink) {
    const round_state_t *round;
    char label[PROTOCOL_LINE_BUFFER_SIZE];

    if (game == NULL) {
        return;
    }

    round = game_get_current_round(game);
    snprintf(label, sizeof(label), "Round %d: %s",
             game->round_number,
             game_view_round_name(round != NULL ? round->category : ROUND_CATEGORY_NONE));
    game_view_broadcast_stage_banner(sink, label);
}

void game_view_broadcast_lobby_status(const game_state_t *game,
                                      const game_view_sink_t *sink) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];

    if (game == NULL) {
        return;
    }

    snprintf(message, sizeof(message),
             "lobby status: %d joined, %d ready, need at least %d ready to start",
             game_count_joined_players(game),
             game_count_ready_players(game),
             PROTOCOL_MIN_PLAYERS);
    game_view_broadcast(sink, message);
}

void game_view_announce_round_start(const game_state_t *game,
                                    const game_view_sink_t *sink) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    size_t i;

    if (game == NULL) {
        return;
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(game, i);
        const char *prompt_text;

        if (!game_view_player_is_active(player)) {
            continue;
        }

        prompt_text = game_get_player_prompt(game, player->player_id);
        if (prompt_text == NULL) {
            continue;
        }

        if (protocol_format_prompt(message, sizeof(message), prompt_text) < 0) {
            continue;
        }

        game_view_send_player(sink, player->player_id, message);
    }
}

void game_view_send_title_prompts(const game_state_t *game,
                                  const game_view_sink_t *sink) {
    const round_state_t *round;
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    size_t i;

    if (game == NULL) {
        return;
    }

    round = game_get_current_round(game);
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(game, i);
        const char *submission_text;

        if (!game_view_player_is_active(player)) {
            continue;
        }

        submission_text = game_get_player_rewrite_submission(game, player->player_id);
        if (submission_text == NULL) {
            continue;
        }

        if (protocol_format_title_prompt(message,
                                         sizeof(message),
                                         game_view_round_category_key(round != NULL ?
                                                                      round->category :
                                                                      ROUND_CATEGORY_NONE),
                                         submission_text) < 0) {
            continue;
        }

        game_view_send_player(sink, player->player_id, message);
    }
}

void game_view_announce_voting_phase(const game_state_t *game,
                                     const game_view_sink_t *sink) {
    round_category_t category;
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    char line[PROTOCOL_LINE_BUFFER_SIZE];
    int reveal_count;
    int option_number;
    size_t i;

    if (game == NULL) {
        return;
    }

    reveal_count = game_get_reveal_entry_count(game);
    if (reveal_count <= 0) {
        return;
    }

    category = game_get_round_category(game);
    game_view_broadcast(sink, "Vote for the funniest entry.");
    game_view_pause(sink);
    for (option_number = 1; option_number <= reveal_count; option_number++) {
        const char *submission_text = game_get_reveal_submission_at(game,
                                                                    (size_t)(option_number - 1));
        const char *title_text = game_get_reveal_title_at(game,
                                                          (size_t)(option_number - 1));

        game_view_broadcast_reveal_option(sink,
                                          category,
                                          option_number,
                                          submission_text,
                                          title_text);
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(game, i);
        int forbidden_option;

        if (!game_view_player_is_active(player)) {
            continue;
        }

        forbidden_option = game_get_player_forbidden_vote_option(game, player->player_id);
        if (forbidden_option > 0) {
            snprintf(line, sizeof(line),
                     "You cannot vote for option %d because that is the title you wrote.",
                     forbidden_option);
            if (protocol_format_info(message, sizeof(message), line) >= 0) {
                game_view_send_player(sink, player->player_id, message);
            }
        }

        snprintf(message, sizeof(message), "%s|%d\n", PROTOCOL_MSG_VOTE, reveal_count);
        game_view_send_player(sink, player->player_id, message);
    }
}

void game_view_broadcast_round_results(const game_state_t *game,
                                       const game_view_sink_t *sink) {
    const round_state_t *round;
    char line[PROTOCOL_LINE_BUFFER_SIZE];
    int reveal_count;
    int option_number;
    int winning_entry_index;
    int winning_title_writer_index;
    const game_player_t *winning_owner;
    const game_player_t *winning_title_writer;

    if (game == NULL) {
        return;
    }

    round = game_get_current_round(game);
    if (round == NULL) {
        return;
    }

    game_view_broadcast(sink, "Here is what everyone came up with:");
    game_view_pause(sink);

    reveal_count = game_get_reveal_entry_count(game);
    for (option_number = 1; option_number <= reveal_count; option_number++) {
        const char *submission_text = game_get_reveal_submission_at(game,
                                                                    (size_t)(option_number - 1));
        const char *title_text = game_get_reveal_title_at(game,
                                                          (size_t)(option_number - 1));

        game_view_broadcast_reveal_option(sink,
                                          round->category,
                                          option_number,
                                          submission_text,
                                          title_text);
    }

    winning_entry_index = round_get_winning_entry_index(round);
    winning_title_writer_index = round_get_winning_title_writer_index(round);
    winning_owner = game_get_player_at(game, (size_t)winning_entry_index);
    winning_title_writer = game_get_player_at(game, (size_t)winning_title_writer_index);
    if (winning_owner != NULL && winning_title_writer != NULL) {
        snprintf(line, sizeof(line), "Winning entry received %d vote(s).",
                 round_get_winning_vote_total(round));
        game_view_broadcast(sink, line);
        game_view_pause(sink);
        snprintf(line, sizeof(line), "Best title by %s (+100)",
                 winning_title_writer->username);
        game_view_broadcast(sink, line);
        game_view_pause(sink);
        snprintf(line, sizeof(line), "Original answer by %s (+25)",
                 winning_owner->username);
        game_view_broadcast(sink, line);
    }

    game_view_broadcast(sink, "End of round.");
}

void game_view_broadcast_scoreboard(const game_state_t *game,
                                    const game_view_sink_t *sink,
                                    const char *label) {
    int player_order[PROTOCOL_MAX_PLAYERS];
    int player_count;
    int i;
    int j;
    char line[PROTOCOL_LINE_BUFFER_SIZE];

    if (game == NULL || label == NULL) {
        return;
    }

    game_view_broadcast(sink, label);
    game_view_pause(sink);

    player_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(game, (size_t)i);

        if (!game_view_player_is_active(player)) {
            continue;
        }

        player_order[player_count] = i;
        player_count++;
    }

    for (i = 0; i < player_count; i++) {
        for (j = i + 1; j < player_count; j++) {
            const game_player_t *left = game_get_player_at(game, (size_t)player_order[i]);
            const game_player_t *right = game_get_player_at(game, (size_t)player_order[j]);

            if (left != NULL && right != NULL && right->score > left->score) {
                int temp = player_order[i];
                player_order[i] = player_order[j];
                player_order[j] = temp;
            }
        }
    }

    for (i = 0; i < player_count; i++) {
        const game_player_t *player = game_get_player_at(game, (size_t)player_order[i]);

        if (player == NULL) {
            continue;
        }

        snprintf(line, sizeof(line), "%d. %s - %d",
                 i + 1, player->username, player->score);
        game_view_broadcast(sink, line);
        game_view_pause(sink);
    }
}

void game_view_broadcast_final_winners(const game_state_t *game,
                                       const game_view_sink_t *sink) {
    int best_score;
    int winner_count;
    size_t i;
    char line[PROTOCOL_LINE_BUFFER_SIZE];

    if (game == NULL) {
        return;
    }

    best_score = -1;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(game, i);

        if (!game_view_player_is_active(player)) {
            continue;
        }

        if (player->score > best_score) {
            best_score = player->score;
        }
    }

    winner_count = 0;
    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(game, i);

        if (!game_view_player_is_active(player) || player->score != best_score) {
            continue;
        }

        winner_count++;
    }

    if (winner_count > 1) {
        strcpy(line, "Co-winners: ");
    } else {
        strcpy(line, "Final winner: ");
    }

    for (i = 0; i < PROTOCOL_MAX_PLAYERS; i++) {
        const game_player_t *player = game_get_player_at(game, i);

        if (!game_view_player_is_active(player) || player->score != best_score) {
            continue;
        }

        if (line[strlen(line) - 1] != ' ') {
            strcat(line, ", ");
        }
        strcat(line, player->username);
    }

    game_view_broadcast(sink, line);
    game_view_pause(sink);
}

static void game_view_broadcast(const game_view_sink_t *sink, const char *text) {
    if (sink == NULL || sink->broadcast_info == NULL || text == NULL) {
        return;
    }

    sink->broadcast_info(sink->context, text);
}

static void game_view_send_player(const game_view_sink_t *sink,
                                  int player_id,
                                  const char *message) {
    if (sink == NULL || sink->send_to_player == NULL || message == NULL) {
        return;
    }

    sink->send_to_player(sink->context, player_id, message);
}

static void game_view_pause(const game_view_sink_t *sink) {
    if (sink == NULL || sink->pause_text_group == NULL) {
        return;
    }

    sink->pause_text_group(sink->context);
}

static void game_view_format_stage_line(char *buffer,
                                        size_t buffer_size,
                                        const char *label) {
    size_t label_len;
    size_t left_padding;
    size_t right_padding;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    if (label == NULL) {
        buffer[0] = '\0';
        return;
    }

    label_len = strlen(label);
    if (label_len > GAME_VIEW_STAGE_WIDTH) {
        label_len = GAME_VIEW_STAGE_WIDTH;
    }

    left_padding = (GAME_VIEW_STAGE_WIDTH - label_len) / 2;
    right_padding = GAME_VIEW_STAGE_WIDTH - label_len - left_padding;

    snprintf(buffer, buffer_size, "%*s%.*s%*s",
             (int)left_padding, "",
             (int)label_len, label,
             (int)right_padding, "");
}

static const char *game_view_title_text(const char *title_text) {
    if (title_text == NULL || title_text[0] == '\0') {
        return "[No title submitted]";
    }

    return title_text;
}

static const char *game_view_round_name(round_category_t category) {
    switch (category) {
        case ROUND_CATEGORY_HEADLINES:
            return "Headlines";
        case ROUND_CATEGORY_CAPTIONS:
            return "Captions";
        case ROUND_CATEGORY_REVIEWS:
            return "Reviews";
        case ROUND_CATEGORY_FORUMS:
            return "Forums";
        case ROUND_CATEGORY_NONE:
        default:
            return "Free Play";
    }
}

static const char *game_view_round_category_key(round_category_t category) {
    switch (category) {
        case ROUND_CATEGORY_HEADLINES:
            return "headlines";
        case ROUND_CATEGORY_CAPTIONS:
            return "captions";
        case ROUND_CATEGORY_REVIEWS:
            return "reviews";
        case ROUND_CATEGORY_FORUMS:
            return "forums";
        case ROUND_CATEGORY_NONE:
        default:
            return "generic";
    }
}

static const char *game_view_round_submission_label(round_category_t category) {
    switch (category) {
        case ROUND_CATEGORY_CAPTIONS:
            return "Post";
        case ROUND_CATEGORY_REVIEWS:
            return "Review";
        case ROUND_CATEGORY_HEADLINES:
        case ROUND_CATEGORY_FORUMS:
        case ROUND_CATEGORY_NONE:
        default:
            return "Comment";
    }
}

static void game_view_broadcast_reveal_option(const game_view_sink_t *sink,
                                              round_category_t category,
                                              int option_number,
                                              const char *submission_text,
                                              const char *title_text) {
    char line[PROTOCOL_LINE_BUFFER_SIZE];

    game_view_broadcast(sink, "");
    game_view_broadcast(sink, "----------------------------------------");
    snprintf(line, sizeof(line), "Option %d", option_number);
    game_view_broadcast(sink, line);
    if (game_view_reveal_twist_first(category)) {
        snprintf(line, sizeof(line), ">>> %s <<<",
                 game_view_title_text(title_text));
        game_view_broadcast(sink, line);
        game_view_pause(sink);
        snprintf(line, sizeof(line), "%s: %s",
                 game_view_round_submission_label(category),
                 submission_text != NULL ? submission_text : "");
        game_view_broadcast(sink, line);
    } else {
        snprintf(line, sizeof(line), "%s: %s",
                 game_view_round_submission_label(category),
                 submission_text != NULL ? submission_text : "");
        game_view_broadcast(sink, line);
        game_view_pause(sink);
        snprintf(line, sizeof(line), "# %s",
                 game_view_title_text(title_text));
        game_view_broadcast(sink, line);
    }
    game_view_pause(sink);
}

static bool game_view_reveal_twist_first(round_category_t category) {
    return category != ROUND_CATEGORY_CAPTIONS;
}

static bool game_view_player_is_active(const game_player_t *player) {
    return player != NULL && player->joined && player->connected;
}
