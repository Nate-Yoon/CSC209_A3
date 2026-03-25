#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * protocol.h
 *
 * Purpose:
 * Shared protocol contract for the CSC209 A3 multiplayer terminal game.
 * This header is the place to centralize stable wire-level decisions such as
 * message names, size limits, and any public parsing/formatting interfaces
 * used by both the client and server.
 *
 * Current scope:
 * Skeleton only. It intentionally defines only high-level constants so the
 * protocol can be documented before helper interfaces are finalized.
 *
 * Likely future helpers, not finalized:
 * - parsing functions for JOIN and READY
 * - formatting functions for WELCOME, ERROR, and lobby updates
 * - validation helpers for usernames and bounded ASCII fields
 */

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
#define PROTOCOL_MSG_WELCOME "WELCOME"
#define PROTOCOL_MSG_ERROR "ERROR"
#define PROTOCOL_MSG_INFO "INFO"

#endif
