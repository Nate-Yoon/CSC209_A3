/*
 * client.c
 *
 * Purpose:
 * Terminal client for the CSC209 A3 multiplayer game.
 * This client hides the plain-text wire protocol from the player by turning
 * normal terminal input into JOIN, READY, SUBMIT, TITLE, and VOTE messages internally.
 */

#define _POSIX_C_SOURCE 200112L

#include "client.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define STRINGIFY_VALUE(x) #x
#define STRINGIFY(x) STRINGIFY_VALUE(x)

enum {
    CLIENT_READ_CHUNK_SIZE = 256
};

typedef struct {
    int player_id;
    bool joined;
    bool join_requested;
    bool awaiting_replay_choice;
    bool replay_choice_sent;
    bool ready_allowed;
    bool ready_sent;
    bool awaiting_submission;
    bool awaiting_title;
    bool awaiting_vote;
    bool prompt_line_active;
    int vote_option_count;
    char username[PROTOCOL_MAX_USERNAME_LEN + 1];
    char title_category[PROTOCOL_MAX_CATEGORY_NAME_LEN + 1];
} client_state_t;

static void client_print_usage(const char *program_name);
static int client_connect(const char *host, const char *port_text);
static int client_send_all(int fd, const char *buffer, size_t len);
static int client_send_join(int fd, const char *username);
static int client_send_ready(int fd);
static int client_send_replay_choice(int fd, bool wants_replay);
static int client_send_submission(int fd, const char *submission);
static int client_send_title(int fd, const char *title_text);
static int client_send_vote(int fd, int option_number);
static int client_run_loop(int fd);
static void client_state_init(client_state_t *state);
static void client_print_separator(void);
static void client_print_welcome_banner(void);
static void client_print_lobby_message(const char *text);
static void client_print_lobby_roster_box(char *roster_text);
static void client_print_join_prompt(void);
static void client_print_username_prompt(void);
static void client_print_ready_prompt(void);
static void client_print_replay_prompt(void);
static void client_print_answer_prompt(void);
static void client_print_title_phase_intro(const client_state_t *state);
static const char *client_title_input_prompt(const client_state_t *state);
static void client_print_title_prompt(const client_state_t *state);
static void client_print_vote_prompt(const client_state_t *state);
static void client_break_prompt_line(client_state_t *state);
static int client_byte_is_allowed(unsigned char byte);
static int client_handle_socket_data(const char *chunk,
                                     ssize_t chunk_len,
                                     char *line_buffer,
                                     size_t *line_len,
                                     client_state_t *state);
static int client_handle_server_line(const char *line, client_state_t *state);
static int client_handle_stdin_line(int fd, client_state_t *state, char *line);
static void client_strip_newline(char *text);

static void client_print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s host [port]\n", program_name);
}

static int client_connect(const char *host, const char *port_text) {
    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *current;
    int fd;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    status = getaddrinfo(host, port_text, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "client: getaddrinfo failed for %s:%s: %s\n",
                host, port_text, gai_strerror(status));
        return -1;
    }

    fd = -1;
    for (current = result; current != NULL; current = current->ai_next) {
        fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static int client_send_all(int fd, const char *buffer, size_t len) {
    size_t total_sent;

    total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(fd, buffer + total_sent, len - total_sent, 0);
        if (sent < 0) {
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}

static int client_send_join(int fd, const char *username) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    int written;

    written = snprintf(message, sizeof(message), "%s|%s\n",
                       PROTOCOL_MSG_JOIN, username);
    if (written < 0 || (size_t)written >= sizeof(message)) {
        return -1;
    }

    return client_send_all(fd, message, (size_t)written);
}

static int client_send_ready(int fd) {
    static const char ready_message[] = PROTOCOL_MSG_READY "\n";

    return client_send_all(fd, ready_message, sizeof(ready_message) - 1);
}

static int client_send_replay_choice(int fd, bool wants_replay) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    int written;

    written = snprintf(message, sizeof(message), "%s|%c\n",
                       PROTOCOL_MSG_REPLAY,
                       wants_replay ? 'y' : 'n');
    if (written < 0 || (size_t)written >= sizeof(message)) {
        return -1;
    }

    return client_send_all(fd, message, (size_t)written);
}

static int client_send_submission(int fd, const char *submission) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    int written;

    written = snprintf(message, sizeof(message), "%s|%s\n",
                       PROTOCOL_MSG_SUBMIT, submission);
    if (written < 0 || (size_t)written >= sizeof(message)) {
        return -1;
    }

    return client_send_all(fd, message, (size_t)written);
}

static int client_send_title(int fd, const char *title_text) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    int written;

    written = snprintf(message, sizeof(message), "%s|%s\n",
                       PROTOCOL_MSG_TITLE, title_text);
    if (written < 0 || (size_t)written >= sizeof(message)) {
        return -1;
    }

    return client_send_all(fd, message, (size_t)written);
}

static int client_send_vote(int fd, int option_number) {
    char message[PROTOCOL_LINE_BUFFER_SIZE];
    int written;

    written = snprintf(message, sizeof(message), "%s|%d\n",
                       PROTOCOL_MSG_VOTE, option_number);
    if (written < 0 || (size_t)written >= sizeof(message)) {
        return -1;
    }

    return client_send_all(fd, message, (size_t)written);
}

static void client_state_init(client_state_t *state) {
    state->player_id = 0;
    state->joined = false;
    state->join_requested = false;
    state->awaiting_replay_choice = false;
    state->replay_choice_sent = false;
    state->ready_allowed = false;
    state->ready_sent = false;
    state->awaiting_submission = false;
    state->awaiting_title = false;
    state->awaiting_vote = false;
    state->prompt_line_active = false;
    state->vote_option_count = 0;
    state->username[0] = '\0';
    strcpy(state->title_category, "generic");
}

static void client_print_separator(void) {
    puts("----------------------------------------------------------------");
}

static void client_print_welcome_banner(void) {
    client_print_separator();
    puts("Welcome to Survive the Internet");
    client_print_separator();
    puts("Survive the Internet is a social game built around funny writing,");
    puts("unexpected twists, and group reactions. Each player gets chances");
    puts("to answer prompts, respond to situations, and react to the");
    puts("choices made by the other players.");
    puts("");
    puts("The fun comes from seeing ordinary answers turned into something");
    puts("more dramatic, ridiculous, or embarrassing once they are shown");
    puts("in a different context later in the round.");
    puts("");
    puts("This version is designed for 3 to 5 players.");
    client_print_separator();
}

static void client_print_lobby_message(const char *text) {
    client_print_separator();
    puts(text);
}

static void client_print_lobby_roster_box(char *roster_text) {
    char *username;
    int slot_number;

    if (roster_text == NULL) {
        return;
    }

    puts("==============================================================");
    puts("Players currently in the lobby:");
    puts("==============================================================");

    slot_number = 1;
    username = strtok(roster_text, "|");
    while (username != NULL) {
        printf("%d. %s\n", slot_number, username);
        slot_number++;
        username = strtok(NULL, "|");
    }

    puts("==============================================================");
}

static void client_print_join_prompt(void) {
    puts("Type \"JOIN\" to join the game.");
    client_print_separator();
    fputs("> ", stdout);
    fflush(stdout);
}

static void client_print_username_prompt(void) {
    client_print_separator();
    fputs("Enter your alphanumeric username: ", stdout);
    fflush(stdout);
}

static void client_print_ready_prompt(void) {
    client_print_separator();
    fputs("Type \"READY\" to signal that you are ready to start the game: ", stdout);
    fflush(stdout);
}

static void client_print_replay_prompt(void) {
    client_print_separator();
    fputs("Would you like to play again (y/n) ", stdout);
    fflush(stdout);
}

static void client_print_answer_prompt(void) {
    client_print_separator();
    fputs("Answer: ", stdout);
    fflush(stdout);
}

static void client_print_title_phase_intro(const client_state_t *state) {
    const char *format;

    if (strcmp(state->title_category, "headlines") == 0) {
        format = "Write a funny news headline for the following comment in %d seconds:\n";
    } else if (strcmp(state->title_category, "captions") == 0) {
        format = "Write a caption / hashtag for the following post in %d seconds:\n";
    } else if (strcmp(state->title_category, "reviews") == 0) {
        format = "Write the product, service, or place being reviewed in %d seconds:\n";
    } else if (strcmp(state->title_category, "forums") == 0) {
        format = "Write a forum thread title / YouTube title for the following comment in %d seconds:\n";
    } else {
        format = "Give the following post a funny title in %d seconds:\n";
    }

    printf(format, PROTOCOL_TITLE_TIMEOUT_SECONDS);
}

static const char *client_title_input_prompt(const client_state_t *state) {
    if (strcmp(state->title_category, "headlines") == 0) {
        return "Headline: ";
    }
    if (strcmp(state->title_category, "captions") == 0) {
        return "Caption / Hashtag: ";
    }
    if (strcmp(state->title_category, "reviews") == 0) {
        return "Reviewed Item: ";
    }
    if (strcmp(state->title_category, "forums") == 0) {
        return "Thread Title: ";
    }

    return "Title: ";
}

static void client_print_title_prompt(const client_state_t *state) {
    client_print_separator();
    fputs(client_title_input_prompt(state), stdout);
    fflush(stdout);
}

static void client_print_vote_prompt(const client_state_t *state) {
    if (state->vote_option_count <= 0) {
        return;
    }

    client_print_separator();
    printf("Vote for the funniest one by typing a number from 1 to %d: ",
           state->vote_option_count);
    fflush(stdout);
}

static void client_break_prompt_line(client_state_t *state) {
    if (state->prompt_line_active) {
        fputc('\n', stdout);
        state->prompt_line_active = false;
    }
}

static int client_byte_is_allowed(unsigned char byte) {
    if (byte == '\n') {
        return 1;
    }

    return byte >= 32 && byte <= 126;
}

static int client_handle_socket_data(const char *chunk,
                                     ssize_t chunk_len,
                                     char *line_buffer,
                                     size_t *line_len,
                                     client_state_t *state) {
    ssize_t i;

    for (i = 0; i < chunk_len; i++) {
        unsigned char byte = (unsigned char)chunk[i];

        if (!client_byte_is_allowed(byte)) {
            fprintf(stderr, "client: received invalid byte from server\n");
            return -1;
        }

        if (*line_len >= PROTOCOL_MAX_LINE_LEN) {
            fprintf(stderr, "client: received overlong line from server\n");
            return -1;
        }

        line_buffer[*line_len] = (char)byte;
        (*line_len)++;

        if (byte != '\n') {
            continue;
        }

        line_buffer[*line_len] = '\0';
        if (client_handle_server_line(line_buffer, state) != 0) {
            return -1;
        }
        *line_len = 0;
    }

    return 0;
}

static int client_handle_server_line(const char *line, client_state_t *state) {
    char roster[PROTOCOL_LINE_BUFFER_SIZE];
    char text[PROTOCOL_LINE_BUFFER_SIZE];
    char prompt[PROTOCOL_MAX_PROMPT_LEN + 1];
    int player_id;

    if (protocol_parse_welcome_id(line, &player_id)) {
        client_break_prompt_line(state);
        state->joined = true;
        state->join_requested = false;
        state->awaiting_replay_choice = false;
        state->replay_choice_sent = false;
        state->ready_allowed = false;
        state->player_id = player_id;
        state->prompt_line_active = false;
        client_print_lobby_message("Joined the lobby.");
        return 0;
    }

    if (protocol_parse_lobby_event_text(line, text, sizeof(text))) {
        client_break_prompt_line(state);
        state->prompt_line_active = false;
        client_print_lobby_message(text);
        return 0;
    }

    if (protocol_parse_lobby_roster(line, roster, sizeof(roster))) {
        client_break_prompt_line(state);
        state->prompt_line_active = false;
        client_print_lobby_roster_box(roster);
        if (!state->ready_sent) {
            state->ready_allowed = true;
            client_print_ready_prompt();
            state->prompt_line_active = true;
        }
        return 0;
    }

    if (protocol_parse_prompt_text(line, prompt, sizeof(prompt))) {
        state->ready_allowed = false;
        state->awaiting_submission = true;
        state->awaiting_title = false;
        state->awaiting_vote = false;
        state->prompt_line_active = false;
        state->vote_option_count = 0;
        printf("Give an answer to the following question in %d seconds:\n",
               PROTOCOL_SUBMISSION_TIMEOUT_SECONDS);
        puts(prompt);
        client_print_answer_prompt();
        state->prompt_line_active = true;
        return 0;
    }

    if (protocol_parse_title_prompt_fields(line,
                                          state->title_category,
                                          sizeof(state->title_category),
                                          prompt,
                                          sizeof(prompt))) {
        state->ready_allowed = false;
        state->awaiting_submission = false;
        state->awaiting_title = true;
        state->awaiting_vote = false;
        state->prompt_line_active = false;
        state->vote_option_count = 0;
        client_print_title_phase_intro(state);
        puts(prompt);
        client_print_title_prompt(state);
        state->prompt_line_active = true;
        return 0;
    }

    if (protocol_parse_vote_open_count(line, &player_id)) {
        state->ready_allowed = false;
        state->awaiting_submission = false;
        state->awaiting_title = false;
        state->awaiting_vote = true;
        state->prompt_line_active = false;
        state->vote_option_count = player_id;
        printf("Voting is open for %d seconds.\n", PROTOCOL_VOTE_TIMEOUT_SECONDS);
        client_print_vote_prompt(state);
        state->prompt_line_active = true;
        return 0;
    }

    if (protocol_parse_vote_rule_option(line, &player_id)) {
        client_break_prompt_line(state);
        state->prompt_line_active = false;
        printf("You cannot vote for option %d because that is the title you wrote.\n",
               player_id);
        return 0;
    }

    if (protocol_parse_round_text(line, text, sizeof(text)) ||
        protocol_parse_game_event_text(line, text, sizeof(text))) {
        client_break_prompt_line(state);
        state->prompt_line_active = false;
        if (strcmp(text, "Would you like to play again (y/n)") == 0) {
            state->awaiting_submission = false;
            state->awaiting_title = false;
            state->awaiting_vote = false;
            state->ready_allowed = false;
            state->awaiting_replay_choice = true;
            state->replay_choice_sent = false;
            client_print_replay_prompt();
            state->prompt_line_active = true;
            return 0;
        }
        puts(text);
        return 0;
    }

    if (protocol_parse_error_text(line, text, sizeof(text))) {
        client_break_prompt_line(state);
        state->prompt_line_active = false;
        printf("Error: %s\n", text);
        if (!state->joined) {
            if (strcmp(text, "game already started") == 0 ||
                strcmp(text, "lobby is full") == 0) {
                return 0;
            }

            if (state->join_requested) {
                client_print_username_prompt();
            } else {
                client_print_join_prompt();
            }
            state->prompt_line_active = true;
        } else if (state->awaiting_submission) {
            client_print_answer_prompt();
            state->prompt_line_active = true;
        } else if (state->awaiting_title) {
            client_print_title_prompt(state);
            state->prompt_line_active = true;
        } else if (state->awaiting_vote) {
            client_print_vote_prompt(state);
            state->prompt_line_active = true;
        } else if (state->awaiting_replay_choice && !state->replay_choice_sent) {
            client_print_replay_prompt();
            state->prompt_line_active = true;
        } else if (!state->ready_sent) {
            client_print_ready_prompt();
            state->prompt_line_active = true;
        }
        return 0;
    }

    client_break_prompt_line(state);
    state->prompt_line_active = false;
    fputs(line, stdout);
    fflush(stdout);
    return 0;
}

static int client_handle_stdin_line(int fd, client_state_t *state, char *line) {
    client_strip_newline(line);

    if (line[0] == '\0') {
        if (!state->joined) {
            client_print_username_prompt();
            state->prompt_line_active = true;
        } else if (state->awaiting_submission) {
            client_print_answer_prompt();
            state->prompt_line_active = true;
        } else if (state->awaiting_title) {
            client_print_title_prompt(state);
            state->prompt_line_active = true;
        } else if (state->awaiting_vote) {
            client_print_vote_prompt(state);
            state->prompt_line_active = true;
        } else if (state->awaiting_replay_choice && !state->replay_choice_sent) {
            client_print_replay_prompt();
            state->prompt_line_active = true;
        } else if (!state->ready_sent) {
            client_print_ready_prompt();
            state->prompt_line_active = true;
        }
        return 0;
    }

    if (!state->joined) {
        if (!state->join_requested) {
            if (strcmp(line, "JOIN") != 0) {
                client_print_join_prompt();
                return 0;
            }

            state->join_requested = true;
            client_print_username_prompt();
            return 0;
        }

        if (!protocol_username_is_valid(line)) {
            printf("Username must be 1-%d alphanumeric characters.\n",
                   PROTOCOL_MAX_USERNAME_LEN);
            client_print_username_prompt();
            state->prompt_line_active = true;
            return 0;
        }

        if (client_send_join(fd, line) != 0) {
            perror("client: send");
            return 1;
        }

        strncpy(state->username, line, sizeof(state->username) - 1);
        state->username[sizeof(state->username) - 1] = '\0';
        puts("Joining lobby...");
        state->prompt_line_active = false;
        return 0;
    }

    if (state->awaiting_replay_choice && !state->replay_choice_sent) {
        bool wants_replay;

        if (strcmp(line, "y") != 0 && strcmp(line, "n") != 0) {
            puts("Type \"y\" to play again or \"n\" to stop.");
            client_print_replay_prompt();
            state->prompt_line_active = true;
            return 0;
        }

        wants_replay = strcmp(line, "y") == 0;
        if (client_send_replay_choice(fd, wants_replay) != 0) {
            perror("client: send");
            return 1;
        }

        state->replay_choice_sent = true;
        state->awaiting_replay_choice = false;
        puts(wants_replay ? "Joined the play-again lobby. Waiting for others..." :
                            "You chose not to play again.");
        state->prompt_line_active = false;
        return 0;
    }

    if (state->awaiting_submission) {
        if (!protocol_player_text_is_valid(line, PROTOCOL_MAX_SUBMISSION_LEN)) {
            printf("Answers must be 1-%d letters or numbers only.\n",
                   PROTOCOL_MAX_SUBMISSION_LEN);
            client_print_answer_prompt();
            state->prompt_line_active = true;
            return 0;
        }

        if (client_send_submission(fd, line) != 0) {
            perror("client: send");
            return 1;
        }

        state->awaiting_submission = false;
        puts("Answer sent. Waiting for the rest of the players...");
        state->prompt_line_active = false;
        return 0;
    }

    if (state->awaiting_title) {
        if (!protocol_player_text_is_valid(line, PROTOCOL_MAX_SUBMISSION_LEN)) {
            printf("Titles must be 1-%d letters or numbers only.\n",
                   PROTOCOL_MAX_SUBMISSION_LEN);
            client_print_title_prompt(state);
            state->prompt_line_active = true;
            return 0;
        }

        if (client_send_title(fd, line) != 0) {
            perror("client: send");
            return 1;
        }

        state->awaiting_title = false;
        puts("Title sent. Waiting for the rest of the players...");
        state->prompt_line_active = false;
        return 0;
    }

    if (state->awaiting_vote) {
        char *endptr;
        long option_number;

        option_number = strtol(line, &endptr, 10);
        if (endptr == line || *endptr != '\0' ||
            option_number < 1 || option_number > state->vote_option_count) {
            printf("Votes must be a number from 1 to %d.\n",
                   state->vote_option_count);
            client_print_vote_prompt(state);
            state->prompt_line_active = true;
            return 0;
        }

        if (client_send_vote(fd, (int)option_number) != 0) {
            perror("client: send");
            return 1;
        }

        state->awaiting_vote = false;
        puts("Vote sent. Waiting for the rest of the players...");
        state->prompt_line_active = false;
        return 0;
    }

    if (!state->ready_sent) {
        if (!state->ready_allowed) {
            client_print_lobby_message("Waiting for enough players to join before readying up.");
            return 0;
        }

        if (strcmp(line, "READY") != 0) {
            puts("Type \"READY\" when you want to mark yourself ready.");
            client_print_ready_prompt();
            state->prompt_line_active = true;
            return 0;
        }

        if (client_send_ready(fd) != 0) {
            perror("client: send");
            return 1;
        }

        state->ready_sent = true;
        puts("Marked ready. Waiting for the game to start...");
        state->prompt_line_active = false;
        return 0;
    }

    puts("Waiting for the current phase to finish...");
    state->prompt_line_active = false;
    return 0;
}

static void client_strip_newline(char *text) {
    size_t len;

    if (text == NULL) {
        return;
    }

    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

static int client_run_loop(int fd) {
    char socket_chunk[CLIENT_READ_CHUNK_SIZE];
    char socket_line_buffer[PROTOCOL_LINE_BUFFER_SIZE];
    size_t socket_line_len;
    int stdin_open;
    client_state_t state;

    socket_line_len = 0;
    socket_line_buffer[0] = '\0';
    stdin_open = 1;
    client_state_init(&state);

    client_print_welcome_banner();
    client_print_join_prompt();

    for (;;) {
        fd_set read_fds;
        int max_fd;
        int ready_count;

        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        max_fd = fd;

        if (stdin_open) {
            FD_SET(STDIN_FILENO, &read_fds);
            if (STDIN_FILENO > max_fd) {
                max_fd = STDIN_FILENO;
            }
        }

        ready_count = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready_count < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("client: select");
            return 1;
        }

        if (FD_ISSET(fd, &read_fds)) {
            ssize_t bytes_read = recv(fd, socket_chunk, sizeof(socket_chunk), 0);
            if (bytes_read < 0) {
                perror("client: recv");
                return 1;
            }

            if (bytes_read == 0) {
                fprintf(stderr, "client: server disconnected\n");
                return 0;
            }

            if (client_handle_socket_data(socket_chunk, bytes_read,
                                          socket_line_buffer,
                                          &socket_line_len,
                                          &state) != 0) {
                return 1;
            }
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &read_fds)) {
            char line[PROTOCOL_LINE_BUFFER_SIZE];
            size_t len;

            if (fgets(line, sizeof(line), stdin) == NULL) {
                stdin_open = 0;
                shutdown(fd, SHUT_WR);
                continue;
            }

            len = strlen(line);
            if (len == 0) {
                continue;
            }

            if (line[len - 1] != '\n') {
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {
                }

                fprintf(stderr, "client: input line too long (max %d)\n",
                        PROTOCOL_MAX_LINE_LEN);
                continue;
            }

            if (len > (size_t)PROTOCOL_MAX_LINE_LEN) {
                fprintf(stderr, "client: input line too long (max %d)\n",
                        PROTOCOL_MAX_LINE_LEN);
                continue;
            }

            if (client_handle_stdin_line(fd, &state, line) != 0) {
                return 1;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    const char *host;
    const char *port_text = STRINGIFY(PORT);
    int fd;

    if (argc < 2 || argc > 3) {
        client_print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    host = argv[1];
    if (argc == 3) {
        port_text = argv[2];
    }

    fd = client_connect(host, port_text);
    if (fd < 0) {
        fprintf(stderr, "client: could not connect to %s:%s\n", host, port_text);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "client: connected to %s:%s\n", host, port_text);
    return client_run_loop(fd);
}
