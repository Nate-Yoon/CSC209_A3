#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * protocol.h
 *
 * Purpose:
 * Shared protocol contract for the CSC209 A3 multiplayer terminal game.
 * This header centralizes stable wire-level decisions such as message names,
 * size limits, and small parsing/formatting helpers used by both ends.
 */

#include <stdbool.h>
#include <stddef.h>

enum {
    PROTOCOL_MIN_PLAYERS = 3,
    PROTOCOL_MAX_PLAYERS = 5,
    PROTOCOL_MAX_USERNAME_LEN = 16,
    PROTOCOL_MAX_SUBMISSION_LEN = 64,
    PROTOCOL_MAX_LINE_LEN = 128,
    PROTOCOL_LINE_BUFFER_SIZE = PROTOCOL_MAX_LINE_LEN + 1
};

#define PROTOCOL_MSG_JOIN "JOIN"
#define PROTOCOL_MSG_READY "READY"
#define PROTOCOL_MSG_SUBMIT "SUBMIT"
#define PROTOCOL_MSG_WELCOME "WELCOME"
#define PROTOCOL_MSG_ERROR "ERROR"
#define PROTOCOL_MSG_INFO "INFO"
#define PROTOCOL_MSG_PROMPT "PROMPT"
#define PROTOCOL_MSG_RESULT "RESULT"

bool protocol_parse_join_username(const char *line,
                                  char *username_out,
                                  size_t username_out_size);
bool protocol_parse_submit_text(const char *line,
                                char *submission_out,
                                size_t submission_out_size);
bool protocol_username_is_valid(const char *username);
bool protocol_submission_is_valid(const char *submission);
int protocol_format_welcome(char *buffer, size_t buffer_size, int player_id);
int protocol_format_error(char *buffer, size_t buffer_size, const char *reason);
int protocol_format_info(char *buffer, size_t buffer_size, const char *text);
int protocol_format_result(char *buffer,
                           size_t buffer_size,
                           const char *username,
                           const char *submission);

#endif
