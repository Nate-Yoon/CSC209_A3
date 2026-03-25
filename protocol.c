/*
 * protocol.c
 *
 * Purpose:
 * Shared plain-text protocol module for the CSC209 A3 client/server game.
 * This file will eventually hold message parsing, message formatting, shared
 * validation rules, and line-length safety checks for all protocol traffic.
 *
 * Current scope:
 * Skeleton only. No protocol logic is implemented yet.
 *
 * Likely future helpers, not finalized:
 * - protocol_parse_* helpers for client -> server messages
 * - protocol_format_* helpers for server -> client messages
 * - shared validation helpers for usernames and single-line payloads
 */

#include "protocol.h"
