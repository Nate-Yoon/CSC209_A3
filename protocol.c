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
static int protocol_checked_snprintf(char *buffer,
                                     size_t buffer_size,
                                     const char *format,
                                     ...);

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

    if (strncmp(line, PROTOCOL_MSG_SUBMIT "|", strlen(PROTOCOL_MSG_SUBMIT) + 1) == 0) {
        return PROTOCOL_TYPE_SUBMIT;
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

bool protocol_parse_submit_text(const char *line,
                                char *submission_out,
                                size_t submission_out_size) {
    return protocol_parse_text_with_prefix(line,
                                           PROTOCOL_MSG_SUBMIT "|",
                                           submission_out,
                                           submission_out_size);
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
    const char *prefix = PROTOCOL_MSG_VOTE "|";
    const char *number_start;
    char *endptr;
    long value;

    if (line == NULL || target_id_out == NULL) {
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

    *target_id_out = (int)value;
    return true;
}

bool protocol_username_is_valid(const char *username) {
    size_t i;
    size_t username_len;

    if (username == NULL) {
        return false;
    }

    username_len = strlen(username);
    if (username_len == 0 || username_len > PROTOCOL_MAX_USERNAME_LEN) {
        return false;
    }

    for (i = 0; i < username_len; i++) {
        unsigned char byte = (unsigned char)username[i];

        if (!isalnum(byte)) {
            return false;
        }
    }

    return true;
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

int protocol_format_welcome(char *buffer, size_t buffer_size, int player_id) {
    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_WELCOME "|%d\n", player_id);
}

int protocol_format_error(char *buffer, size_t buffer_size, const char *reason) {
    if (buffer == NULL || buffer_size == 0 || reason == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_ERROR "|%s\n", reason);
}

int protocol_format_info(char *buffer, size_t buffer_size, const char *text) {
    if (buffer == NULL || buffer_size == 0 || text == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_INFO "|%s\n", text);
}

int protocol_format_result(char *buffer,
                           size_t buffer_size,
                           const char *username,
                           const char *submission) {
    if (buffer == NULL || buffer_size == 0 ||
        username == NULL || submission == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_RESULT "|%s|%s\n",
                                     username, submission);
}

int protocol_format_winner(char *buffer, size_t buffer_size, const char *username) {
    if (buffer == NULL || buffer_size == 0 || username == NULL) {
        return -1;
    }

    return protocol_checked_snprintf(buffer, buffer_size,
                                     PROTOCOL_MSG_WINNER "|%s\n", username);
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

    if (text_len == 0 || text_len >= text_out_size) {
        return false;
    }

    memcpy(text_out, text_start, text_len);
    text_out[text_len] = '\0';
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
