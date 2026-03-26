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
#include <stdio.h>
#include <string.h>

bool protocol_parse_join_username(const char *line,
                                  char *username_out,
                                  size_t username_out_size) {
    const char *prefix = PROTOCOL_MSG_JOIN "|";
    const char *username_start;
    size_t username_len;

    if (line == NULL || username_out == NULL || username_out_size == 0) {
        return false;
    }

    if (strncmp(line, prefix, strlen(prefix)) != 0) {
        return false;
    }

    username_start = line + strlen(prefix);
    username_len = strcspn(username_start, "\n");

    if (username_start[username_len] != '\n') {
        return false;
    }

    if (username_len == 0 || username_len >= username_out_size) {
        return false;
    }

    memcpy(username_out, username_start, username_len);
    username_out[username_len] = '\0';
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

int protocol_format_welcome(char *buffer, size_t buffer_size, int player_id) {
    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    return snprintf(buffer, buffer_size,
                    PROTOCOL_MSG_WELCOME "|%d\n", player_id);
}

int protocol_format_error(char *buffer, size_t buffer_size, const char *reason) {
    if (buffer == NULL || buffer_size == 0 || reason == NULL) {
        return -1;
    }

    return snprintf(buffer, buffer_size,
                    PROTOCOL_MSG_ERROR "|%s\n", reason);
}

int protocol_format_info(char *buffer, size_t buffer_size, const char *text) {
    if (buffer == NULL || buffer_size == 0 || text == NULL) {
        return -1;
    }

    return snprintf(buffer, buffer_size,
                    PROTOCOL_MSG_INFO "|%s\n", text);
}
