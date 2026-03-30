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
    PROTOCOL_MAX_CATEGORY_NAME_LEN = 16,
    PROTOCOL_MAX_SUBMISSION_LEN = 96,
    PROTOCOL_MAX_LINE_LEN = 128,
    PROTOCOL_MAX_PROMPT_LEN = PROTOCOL_MAX_LINE_LEN - 8,
    PROTOCOL_LINE_BUFFER_SIZE = PROTOCOL_MAX_LINE_LEN + 1,
    PROTOCOL_SUBMISSION_TIMEOUT_SECONDS = 45,
    PROTOCOL_TITLE_TIMEOUT_SECONDS = 60,
    PROTOCOL_VOTE_TIMEOUT_SECONDS = 30
};

typedef enum {
    PROTOCOL_TYPE_UNKNOWN = 0,
    PROTOCOL_TYPE_JOIN,
    PROTOCOL_TYPE_READY,
    PROTOCOL_TYPE_SUBMIT,
    PROTOCOL_TYPE_TITLE,
    PROTOCOL_TYPE_REWRITE,
    PROTOCOL_TYPE_VOTE
} protocol_message_type_t;

#define PROTOCOL_MSG_JOIN "JOIN"
#define PROTOCOL_MSG_READY "READY"
#define PROTOCOL_MSG_SUBMIT "SUBMIT"
#define PROTOCOL_MSG_TITLE "TITLE"
#define PROTOCOL_MSG_REWRITE "REWRITE"
#define PROTOCOL_MSG_VOTE "VOTE"
#define PROTOCOL_MSG_WELCOME "WELCOME"
#define PROTOCOL_MSG_ERROR "ERROR"
#define PROTOCOL_MSG_INFO "INFO"
#define PROTOCOL_MSG_PROMPT "PROMPT"
#define PROTOCOL_MSG_RESULT "RESULT"
#define PROTOCOL_MSG_WINNER "WINNER"

protocol_message_type_t protocol_identify_message(const char *line);
bool protocol_parse_join_username(const char *line,
                                  char *username_out,
                                  size_t username_out_size);
bool protocol_parse_welcome_id(const char *line, int *player_id_out);
bool protocol_parse_submit_text(const char *line,
                                char *submission_out,
                                size_t submission_out_size);
bool protocol_parse_title_text(const char *line,
                               char *title_out,
                               size_t title_out_size);
bool protocol_parse_title_prompt_fields(const char *line,
                                        char *category_out,
                                        size_t category_out_size,
                                        char *text_out,
                                        size_t text_out_size);
bool protocol_parse_rewrite_text(const char *line,
                                 char *rewrite_out,
                                 size_t rewrite_out_size);
bool protocol_parse_vote_target(const char *line, int *target_id_out);
bool protocol_parse_info_text(const char *line,
                              char *text_out,
                              size_t text_out_size);
bool protocol_parse_error_text(const char *line,
                               char *text_out,
                               size_t text_out_size);
bool protocol_parse_prompt_text(const char *line,
                                char *prompt_out,
                                size_t prompt_out_size);
bool protocol_parse_result_fields(const char *line,
                                  char *username_out,
                                  size_t username_out_size,
                                  char *submission_out,
                                  size_t submission_out_size);
bool protocol_parse_winner_username(const char *line,
                                    char *username_out,
                                    size_t username_out_size);
bool protocol_username_is_valid(const char *username);
bool protocol_player_text_is_valid(const char *text, size_t max_len);
bool protocol_submission_is_valid(const char *submission);
int protocol_format_welcome(char *buffer, size_t buffer_size, int player_id);
int protocol_format_error(char *buffer, size_t buffer_size, const char *reason);
int protocol_format_info(char *buffer, size_t buffer_size, const char *text);
int protocol_format_prompt(char *buffer, size_t buffer_size, const char *prompt_text);
int protocol_format_title(char *buffer, size_t buffer_size, const char *title_text);
int protocol_format_title_prompt(char *buffer,
                                 size_t buffer_size,
                                 const char *category,
                                 const char *text);
int protocol_format_result(char *buffer,
                           size_t buffer_size,
                           const char *username,
                           const char *submission);
int protocol_format_winner(char *buffer, size_t buffer_size, const char *username);

#endif
