#include "uci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void apply_uci_move(Board *board, const char *move_text) {
    Move move;
    if (!movegen_find_legal_move(board, move_text, &move)) {
        return;
    }
    Undo undo;
    if (!board_make_move(board, move, &undo)) {
        return;
    }
}

static void apply_position(Board *board, char *line) {
    char *tokens[256];
    int token_count = 0;
    for (char *token = strtok(line, " \t\r\n"); token != NULL && token_count < 256; token = strtok(NULL, " \t\r\n")) {
        tokens[token_count++] = token;
    }

    if (token_count < 2) {
        return;
    }

    int move_start = 0;
    if (strcmp(tokens[1], "startpos") == 0) {
        board_set_startpos(board);
        move_start = 2;
    } else if (strcmp(tokens[1], "fen") == 0) {
        if (token_count < 8) {
            return;
        }
        char fen[512] = {0};
        snprintf(fen, sizeof(fen), "%s %s %s %s %s %s", tokens[2], tokens[3], tokens[4], tokens[5], tokens[6], tokens[7]);
        if (!board_set_fen(board, fen)) {
            board_set_startpos(board);
        }
        move_start = 8;
    } else {
        return;
    }

    if (move_start < token_count && strcmp(tokens[move_start], "moves") == 0) {
        ++move_start;
    }

    for (int i = move_start; i < token_count; ++i) {
        apply_uci_move(board, tokens[i]);
    }
}

static void handle_go(Board *board, char *line) {
    SearchLimits limits;
    memset(&limits, 0, sizeof(limits));

    for (char *token = strtok(line, " \t\r\n"); token != NULL; token = strtok(NULL, " \t\r\n")) {
        if (strcmp(token, "depth") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                int parsed_depth = atoi(value);
                if (parsed_depth > 0) {
                    limits.depth = parsed_depth;
                }
            }
        } else if (strcmp(token, "wtime") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                limits.wtime_ms = atoi(value);
            }
        } else if (strcmp(token, "btime") == 0) {
            char *value = strtok(NULL, " \t\r\n");
            if (value != NULL) {
                limits.btime_ms = atoi(value);
            }
        }
    }

    Move best = think(board, &limits);
    if (best == MOVE_NONE) {
        printf("bestmove 0000\n");
        fflush(stdout);
        return;
    }

    char buffer[6];
    move_to_string(best, buffer);
    printf("bestmove %s\n", buffer);
    fflush(stdout);
}

void uci_loop(void) {
    Board board;
    board_init(&board);

    char line[4096];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strncmp(line, "uci", 3) == 0 && (line[3] == '\0' || line[3] == ' ' || line[3] == '\t' || line[3] == '\r' || line[3] == '\n')) {
            printf("id name Cepimetheus\n");
            printf("id author  George Bland\n");
            printf("uciok\n");
            fflush(stdout);
            continue;
        }

        if (strncmp(line, "isready", 7) == 0) {
            printf("readyok\n");
            fflush(stdout);
            continue;
        }

        if (strncmp(line, "ucinewgame", 10) == 0) {
            board_set_startpos(&board);
            continue;
        }

        if (strncmp(line, "position", 8) == 0) {
            apply_position(&board, line);
            continue;
        }

        if (strncmp(line, "go", 2) == 0 && (line[2] == '\0' || line[2] == ' ' || line[2] == '\t' || line[2] == '\r' || line[2] == '\n')) {
            handle_go(&board, line);
            continue;
        }

        if (strncmp(line, "stop", 4) == 0) {
            continue;
        }

        if (strncmp(line, "quit", 4) == 0) {
            break;
        }
    }
}
