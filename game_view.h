#ifndef GAME_VIEW_H
#define GAME_VIEW_H

/*
 * game_view.h
 *
 * Purpose:
 * Player-facing presentation helpers for the CSC209 A3 server.
 * This module owns terminal-readable formatting for stage banners, phase
 * announcements, reveals, and scoreboards while the server keeps ownership of
 * sockets, broadcasting, and scheduling.
 */

#include "game.h"

typedef void (*game_view_send_to_player_fn)(void *context,
                                            int player_id,
                                            const char *message);
typedef void (*game_view_broadcast_info_fn)(void *context, const char *text);
typedef void (*game_view_pause_fn)(void *context);

typedef struct {
    void *context;
    game_view_send_to_player_fn send_to_player;
    game_view_broadcast_info_fn broadcast_info;
    game_view_pause_fn pause_text_group;
} game_view_sink_t;

void game_view_broadcast_stage_banner(const game_view_sink_t *sink,
                                      const char *label);
void game_view_broadcast_round_intro(const game_state_t *game,
                                     const game_view_sink_t *sink);
void game_view_broadcast_lobby_status(const game_state_t *game,
                                      const game_view_sink_t *sink);
void game_view_announce_round_start(const game_state_t *game,
                                    const game_view_sink_t *sink);
void game_view_send_title_prompts(const game_state_t *game,
                                  const game_view_sink_t *sink);
void game_view_announce_voting_phase(const game_state_t *game,
                                     const game_view_sink_t *sink);
void game_view_broadcast_round_results(const game_state_t *game,
                                       const game_view_sink_t *sink);
void game_view_broadcast_scoreboard(const game_state_t *game,
                                    const game_view_sink_t *sink,
                                    const char *label);
void game_view_broadcast_final_winners(const game_state_t *game,
                                       const game_view_sink_t *sink);

#endif
