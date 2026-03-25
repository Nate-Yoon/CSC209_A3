/*
 * game.c
 *
 * Purpose:
 * Server-owned game-state module for the CSC209 A3 project.
 * This file will eventually contain the authoritative round/phase logic that
 * sits above the socket layer and below the client presentation layer.
 *
 * Current scope:
 * Skeleton only. No game logic or phase transitions are implemented yet.
 *
 * Likely future helpers, not finalized:
 * - game_init and reset helpers
 * - lobby/start state transitions
 * - round/phase progression helpers
 * - validation for gameplay actions after the lobby phase exists
 */

#include "game.h"
