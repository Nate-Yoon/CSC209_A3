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
    PROTOCOL_REPLAY_COUNTDOWN_SECONDS = 60,
    PROTOCOL_SUBMISSION_TIMEOUT_SECONDS = 45,
    PROTOCOL_TITLE_TIMEOUT_SECONDS = 60,
    PROTOCOL_VOTE_TIMEOUT_SECONDS = 30
};

typedef enum {
    PROTOCOL_TYPE_UNKNOWN = 0,
    PROTOCOL_TYPE_JOIN,
    PROTOCOL_TYPE_READY,
    PROTOCOL_TYPE_REPLAY,
    PROTOCOL_TYPE_SUBMIT,
    PROTOCOL_TYPE_TITLE,
    PROTOCOL_TYPE_VOTE
} protocol_message_type_t;

#define PROTOCOL_MSG_JOIN "JOIN"
#define PROTOCOL_MSG_READY "READY"
#define PROTOCOL_MSG_REPLAY "REPLAY"
#define PROTOCOL_MSG_SUBMIT "SUBMIT"
#define PROTOCOL_MSG_TITLE "TITLE"
#define PROTOCOL_MSG_VOTE "VOTE"
#define PROTOCOL_MSG_WELCOME "WELCOME"
#define PROTOCOL_MSG_LOBBY_EVENT "LOBBY_EVENT"
#define PROTOCOL_MSG_LOBBY_ROSTER "LOBBY_ROSTER"
#define PROTOCOL_MSG_ERROR "ERROR"
#define PROTOCOL_MSG_PROMPT "PROMPT"
#define PROTOCOL_MSG_TITLE_PROMPT "TITLE_PROMPT"
#define PROTOCOL_MSG_VOTE_OPEN "VOTE_OPEN"
#define PROTOCOL_MSG_VOTE_RULE "VOTE_RULE"
#define PROTOCOL_MSG_ROUND_TEXT "ROUND_TEXT"
#define PROTOCOL_MSG_GAME_EVENT "GAME_EVENT"

/*
 * Wire-level message contract
 *
 * Client -> server
 * JOIN|<username>\n
 * READY\n
 * REPLAY|<y-or-n>\n
 * SUBMIT|<answer text>\n
 * TITLE|<title text>\n
 * VOTE|<option number>\n
 *
 * Server -> client
 * WELCOME|<player id>\n
 * LOBBY_EVENT|<text>\n
 * LOBBY_ROSTER|<username>|<username>|...\n
 * PROMPT|<prompt text>\n
 * TITLE_PROMPT|<category>|<submission text>\n
 * VOTE_RULE|<forbidden option>\n
 * VOTE_OPEN|<option count>\n
 * ROUND_TEXT|<text>\n
 * GAME_EVENT|<text>\n
 * ERROR|<reason>\n
 *
 * Invalid-message behavior
 * - Unknown headers are rejected by the server with ERROR|unsupported message.
 * - Known headers with malformed payloads are rejected with a message-specific
 *   ERROR response such as invalid JOIN format or invalid vote format.
 * - Client-side parse helpers return false on malformed server messages; the
 *   client falls back to printing the raw line instead of guessing.
 */

protocol_message_type_t protocol_identify_message(const char *line);
bool protocol_parse_join_username(const char *line,
                                  char *username_out,
                                  size_t username_out_size);
bool protocol_parse_welcome_id(const char *line, int *player_id_out);
bool protocol_parse_lobby_event_text(const char *line,
                                     char *text_out,
                                     size_t text_out_size);
bool protocol_parse_lobby_roster(const char *line,
                                 char *roster_out,
                                 size_t roster_out_size);
bool protocol_parse_replay_choice(const char *line, bool *wants_replay_out);
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
bool protocol_parse_vote_target(const char *line, int *target_id_out);
bool protocol_parse_vote_open_count(const char *line, int *option_count_out);
bool protocol_parse_vote_rule_option(const char *line, int *option_number_out);
bool protocol_parse_round_text(const char *line,
                               char *text_out,
                               size_t text_out_size);
bool protocol_parse_game_event_text(const char *line,
                                    char *text_out,
                                    size_t text_out_size);
bool protocol_parse_error_text(const char *line,
                               char *text_out,
                               size_t text_out_size);
bool protocol_parse_prompt_text(const char *line,
                                char *prompt_out,
                                size_t prompt_out_size);
bool protocol_username_is_valid(const char *username);
bool protocol_player_text_is_valid(const char *text, size_t max_len);
bool protocol_submission_is_valid(const char *submission);
int protocol_format_welcome(char *buffer, size_t buffer_size, int player_id);
int protocol_format_lobby_event(char *buffer, size_t buffer_size, const char *text);
int protocol_format_lobby_roster(char *buffer,
                                 size_t buffer_size,
                                 const char *const *usernames,
                                 size_t username_count);
int protocol_format_error(char *buffer, size_t buffer_size, const char *reason);
int protocol_format_prompt(char *buffer, size_t buffer_size, const char *prompt_text);
int protocol_format_title_prompt(char *buffer,
                                 size_t buffer_size,
                                 const char *category,
                                 const char *text);
int protocol_format_vote_open(char *buffer, size_t buffer_size, int option_count);
int protocol_format_vote_rule(char *buffer, size_t buffer_size, int option_number);
int protocol_format_round_text(char *buffer, size_t buffer_size, const char *text);
int protocol_format_game_event(char *buffer, size_t buffer_size, const char *text);

#endif
