/*
 * protocol.c
 *
 * Purpose:
 * Shared plain-text protocol helpers for the CSC209 A3 client/server game.
 * This module handles the small pieces of parsing, formatting, and validation
 * that both the server and client need to agree on.
 */

#include "protocol.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool protocol_parse_text_with_prefix(const char *line,
                                            const char *prefix,
                                            char *text_out,
                                            size_t text_out_size);
static bool protocol_parse_two_fields(const char *line,
                                      const char *prefix,
                                      char *first_out,
                                      size_t first_out_size,
                                      char *second_out,
                                      size_t second_out_size);
static bool protocol_parse_int_field(const char *line,
                                     const char *prefix,
                                     int *value_out);
static int protocol_checked_snprintf(char *buffer,
                                     size_t buffer_size,
                                     const char *format,
                                     ...);
static bool protocol_text_is_alnum_only(const char *text, size_t max_len);
static bool protocol_text_is_alnum_space_only(const char *text, size_t max_len);

protocol_message_type_t protocol_identify_message(const char *line) {
    if (line == NULL) {
        return PROTOCOL_TYPE_UNKNOWN;
    }

    if (strncmp(line, PROTOCOL_MSG_JOIN "|", strlen(PROTOCOL_MSG_JOIN) + 1) == 0) {
        return PROTOCOL_TYPE_JOIN;
    }

    if (strcmp(line, PROTOCOL_MSG_READY "\n") == 0) {
        return PROTOCOL_TYPE_READY;
    }

    if (strncmp(line, PROTOCOL_MSG_REPLAY "|", strlen(PROTOCOL_MSG_REPLAY) + 1) == 0) {
        return PROTOCOL_TYPE_REPLAY;
    }

    if (strncmp(line, PROTOCOL_MSG_SUBMIT "|", strlen(PROTOCOL_MSG_SUBMIT) + 1) == 0) {
        return PROTOCOL_TYPE_SUBMIT;
    }

    if (strncmp(line, PROTOCOL_MSG_TITLE "|", strlen(PROTOCOL_MSG_TITLE) + 1) == 0) {
        return PROTOCOL_TYPE_TITLE;
    }

    if (strncmp(line, PROTOCOL_MSG_REWRITE "|", strlen(PROTOCOL_MSG_REWRITE) + 1) == 0) {
        return PROTOCOL_TYPE_REWRITE;
    }

    if (strncmp(line, PROTOCOL_MSG_VOTE "|", strlen(PROTOCOL_MSG_VOTE) + 1) == 0) {
        return PROTOCOL_TYPE_VOTE;
    }

    return PROTOCOL_TYPE_UNKNOWN;
}

bool protocol_parse_join_username(const char *line,
                                  char *username_out,
                                  size_t username_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_JOIN "|",
                                           username_out,
                                           username_out_size);
}

bool protocol_parse_welcome_id(const char *line, int *player_id_out) {
    return protocol_parse_int_field(line, PROTOCOL_MSG_WELCOME "|", player_id_out);
}

bool protocol_parse_lobby_event_text(const char *line,
                                     char *text_out,
                                     size_t text_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_LOBBY_EVENT "|",
                                           text_out,
                                           text_out_size);
}

bool protocol_parse_lobby_roster(const char *line,
                                 char *roster_out,
                                 size_t roster_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_LOBBY_ROSTER "|",
                                           roster_out,
                                           roster_out_size);
}

bool protocol_parse_replay_choice(const char *line, bool *wants_replay_out) {
    char choice[2];

    if (wants_replay_out == NULL) {
        return false;
    }

    if (!protocol_parse_text_with_prefix(line,
                                         PROTOCOL_MSG_REPLAY "|",
                                         choice,
                                         sizeof(choice))) {
        return false;
    }

    if (choice[0] == 'y' && choice[1] == '\0') {
        *wants_replay_out = true;
        return true;
    }

    if (choice[0] == 'n' && choice[1] == '\0') {
        *wants_replay_out = false;
        return true;
    }

    return false;
}

bool protocol_parse_submit_text(const char *line,
                                char *submission_out,
                                size_t submission_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_SUBMIT "|",
                                           submission_out,
                                           submission_out_size);
}

bool protocol_parse_title_text(const char *line,
                               char *title_out,
                               size_t title_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_TITLE "|",
                                           title_out,
                                           title_out_size);
}

bool protocol_parse_title_prompt_fields(const char *line,
                                        char *category_out,
                                        size_t category_out_size,
                                        char *text_out,
                                        size_t text_out_size) {
    return protocol_parse_two_fields(line,
                                     PROTOCOL_MSG_TITLE_PROMPT "|",
                                     category_out,
                                     category_out_size,
                                     text_out,
                                     text_out_size);
}

bool protocol_parse_rewrite_text(const char *line,
                                 char *rewrite_out,
                                 size_t rewrite_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_REWRITE "|",
                                           rewrite_out,
                                           rewrite_out_size);
}

bool protocol_parse_vote_target(const char *line, int *target_id_out) {
    return protocol_parse_int_field(line, PROTOCOL_MSG_VOTE "|", target_id_out);
}

bool protocol_parse_vote_open_count(const char *line, int *option_count_out) {
    return protocol_parse_int_field(line, PROTOCOL_MSG_VOTE_OPEN "|", option_count_out);
}

bool protocol_parse_vote_rule_option(const char *line, int *option_number_out) {
    return protocol_parse_int_field(line, PROTOCOL_MSG_VOTE_RULE "|", option_number_out);
}

bool protocol_parse_round_text(const char *line,
                               char *text_out,
                               size_t text_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_ROUND_TEXT "|",
                                           text_out,
                                           text_out_size);
}

bool protocol_parse_game_event_text(const char *line,
                                    char *text_out,
                                    size_t text_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_GAME_EVENT "|",
                                           text_out,
                                           text_out_size);
}

bool protocol_parse_error_text(const char *line,
                               char *text_out,
                               size_t text_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_ERROR "|",
                                           text_out,
                                           text_out_size);
}

bool protocol_parse_prompt_text(const char *line,
                                char *prompt_out,
                                size_t prompt_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_PROMPT "|",
                                           prompt_out,
                                           prompt_out_size);
}

bool protocol_username_is_valid(const char *username) {
    return protocol_text_is_alnum_only(username, PROTOCOL_MAX_USERNAME_LEN);
}

bool protocol_player_text_is_valid(const char *text, size_t max_len) {
    return protocol_text_is_alnum_space_only(text, max_len);
}

bool protocol_submission_is_valid(const char *submission) {
    size_t i;
    size_t submission_len;

    if (submission == NULL) {
        return false;
    }

    submission_len = strlen(submission);
    if (submission_len == 0 || submission_len > PROTOCOL_MAX_SUBMISSION_LEN) {
        return false;
    }

    for (i = 0; i < submission_len; i++) {
        unsigned char byte = (unsigned char)submission[i];

        if (byte < 32 || byte > 126 || byte == '|') {
            return false;
        }
    }

    return true;
}

static bool protocol_text_is_alnum_only(const char *text, size_t max_len) {
    size_t i;
    size_t text_len;

    if (text == NULL) {
        return false;
    }

    text_len = strlen(text);
    if (text_len == 0 || text_len > max_len) {
        return false;
    }

    for (i = 0; i < text_len; i++) {
        unsigned char byte = (unsigned char)text[i];

        if (!isalnum(byte)) {
            return false;
        }
    }

    return true;
}

static bool protocol_text_is_alnum_space_only(const char *text, size_t max_len) {
    size_t i;
    size_t text_len;

    if (text == NULL) {
        return false;
    }

    text_len = strlen(text);
    if (text_len == 0 || text_len > max_len) {
        return false;
    }

    for (i = 0; i < text_len; i++) {
        unsigned char byte = (unsigned char)text[i];

        if (!isalnum(byte) && byte != ' ') {
            return false;
        }
    }

    return true;
}

int protocol_format_welcome(char *buffer, size_t buffer_size, int player_id) {
    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_WELCOME "|%d\n", player_id);
}

int protocol_format_lobby_event(char *buffer, size_t buffer_size, const char *text) {
    if (buffer == NULL || buffer_size == 0 || text == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_LOBBY_EVENT "|%s\n", text);
}

int protocol_format_lobby_roster(char *buffer,
                                 size_t buffer_size,
                                 const char *const *usernames,
                                 size_t username_count) {
    size_t used;
    size_t i;

    if (buffer == NULL || buffer_size == 0 || usernames == NULL || username_count == 0) {
        return -1;
    }

    used = (size_t)snprintf(buffer, buffer_size, "%s", PROTOCOL_MSG_LOBBY_ROSTER);
    if (used >= buffer_size) {
        return -1;
    }

    for (i = 0; i < username_count; i++) {
        int written;

        if (usernames[i] == NULL) {
            return -1;
        }

        written = snprintf(buffer + used, buffer_size - used, "|%s", usernames[i]);
        if (written < 0 || (size_t)written >= buffer_size - used) {
            return -1;
        }
        used += (size_t)written;
    }

    if (used + 2 > buffer_size) {
        return -1;
    }

    buffer[used++] = '\n';
    buffer[used] = '\0';
    return (int)used;
}

int protocol_format_error(char *buffer, size_t buffer_size, const char *reason) {
    if (buffer == NULL || buffer_size == 0 || reason == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_ERROR "|%s\n", reason);
}

int protocol_format_prompt(char *buffer, size_t buffer_size, const char *prompt_text) {
    if (buffer == NULL || buffer_size == 0 || prompt_text == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_PROMPT "|%s\n", prompt_text);
}

int protocol_format_title_prompt(char *buffer,
                                 size_t buffer_size,
                                 const char *category,
                                 const char *text) {
    if (buffer == NULL || buffer_size == 0 || category == NULL || text == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_TITLE_PROMPT "|%s|%s\n",
                                     category, text);
}

int protocol_format_vote_open(char *buffer, size_t buffer_size, int option_count) {
    if (buffer == NULL || buffer_size == 0 || option_count <= 0) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_VOTE_OPEN "|%d\n", option_count);
}

int protocol_format_vote_rule(char *buffer, size_t buffer_size, int option_number) {
    if (buffer == NULL || buffer_size == 0 || option_number <= 0) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_VOTE_RULE "|%d\n", option_number);
}

int protocol_format_round_text(char *buffer, size_t buffer_size, const char *text) {
    if (buffer == NULL || buffer_size == 0 || text == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_ROUND_TEXT "|%s\n", text);
}

int protocol_format_game_event(char *buffer, size_t buffer_size, const char *text) {
    if (buffer == NULL || buffer_size == 0 || text == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_GAME_EVENT "|%s\n", text);
}

static bool protocol_parse_text_with_prefix(const char *line,
                                            const char *prefix,
                                            char *text_out,
                                            size_t text_out_size) {
    const char *text_start;
    size_t text_len;

    if (line == NULL || prefix == NULL || text_out == NULL || text_out_size == 0) {
        return false;
    }

    if (strncmp(line, prefix, strlen(prefix)) != 0) {
        return false;
    }

    text_start = line + strlen(prefix);
    text_len = strcspn(text_start, "\n");

    if (text_start[text_len] != '\n') {
        return false;
    }

    if (text_len >= text_out_size) {
        return false;
    }

    memcpy(text_out, text_start, text_len);
    text_out[text_len] = '\0';
    return true;
}

static bool protocol_parse_two_fields(const char *line,
                                      const char *prefix,
                                      char *first_out,
                                      size_t first_out_size,
                                      char *second_out,
                                      size_t second_out_size) {
    const char *first_start;
    const char *separator;
    const char *second_start;
    size_t first_len;
    size_t second_len;

    if (line == NULL || prefix == NULL ||
        first_out == NULL || second_out == NULL ||
        first_out_size == 0 || second_out_size == 0) {
        return false;
    }

    if (strncmp(line, prefix, strlen(prefix)) != 0) {
        return false;
    }

    first_start = line + strlen(prefix);
    separator = strchr(first_start, '|');
    if (separator == NULL) {
        return false;
    }

    second_start = separator + 1;
    second_len = strcspn(second_start, "\n");
    if (second_start[second_len] != '\n') {
        return false;
    }

    first_len = (size_t)(separator - first_start);
    if (first_len == 0 || first_len >= first_out_size ||
        second_len == 0 || second_len >= second_out_size) {
        return false;
    }

    memcpy(first_out, first_start, first_len);
    first_out[first_len] = '\0';
    memcpy(second_out, second_start, second_len);
    second_out[second_len] = '\0';
    return true;
}

static bool protocol_parse_int_field(const char *line,
                                     const char *prefix,
                                     int *value_out) {
    const char *number_start;
    char *endptr;
    long value;

    if (line == NULL || prefix == NULL || value_out == NULL) {
        return false;
    }

    if (strncmp(line, prefix, strlen(prefix)) != 0) {
        return false;
    }

    number_start = line + strlen(prefix);
    errno = 0;
    value = strtol(number_start, &endptr, 10);
    if (endptr == number_start || strcmp(endptr, "\n") != 0) {
        return false;
    }

    if (errno == ERANGE || value <= 0 || value > INT_MAX) {
        return false;
    }

    *value_out = (int)value;
    return true;
}


static int protocol_checked_snprintf(char *buffer,
                                     size_t buffer_size,
                                     const char *format,
                                     ...) {
    int written;
    va_list args;

    if (buffer == NULL || buffer_size == 0 || format == NULL) {
        return -1;
    }

    va_start(args, format);
    written = vsnprintf(buffer, buffer_size, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }

    return written;
}
