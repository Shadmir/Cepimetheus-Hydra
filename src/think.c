#include "think.h"
#include <stdio.h>
#include "float.h"
#include <stdlib.h>
#include <time.h>

typedef struct {
    float score;
    Move move;
} SearchResult;

typedef struct {
    Move move;
    int score;
} ScoredMove;

static int score_to_cp(float score) {
    if (score >= 0.0f) {
        return (int)(score + 0.5f);
    }

    return (int)(score - 0.5f);
}

static void print_move_info(int depth, int move_number, Move move, float score) {
    char move_buffer[6];
    move_to_string(move, move_buffer);
    printf("info depth %d currmove %s currmovenumber %d score cp %d\n",
           depth,
           move_buffer,
           move_number,
           score_to_cp(score));
    fflush(stdout);
}

static void print_depth_info(int depth, SearchResult result) {
    printf("info depth %d score cp %d", depth, score_to_cp(result.score));
    if (result.move != MOVE_NONE) {
        char move_buffer[6];
        move_to_string(result.move, move_buffer);
        printf(" pv %s", move_buffer);
    }
    printf("\n");
    fflush(stdout);
}

static const int piece_values[6] = {
    100, /* Pawn */
    310, /* Knight */
    320, /* Bishop */
    500, /* Rook */
    950, /* Queen */
    0    /* King */
};

/* Count the number of set bits in a 64-bit bitboard. */
static int popcount_u64(U64 bb) {
    int count = 0;

    /* Repeatedly remove the least-significant set bit until empty. */
    while (bb) {
        bitboard_pop_lsb(&bb);
        ++count;
    }

    return count;
}

static int count_pieces(Board *board) {
    if (board == NULL) {
        return 0;
    }

    int count = 0;
    for (int piece = 0; piece < PIECE_NB; ++piece) {
        count += popcount_u64(board->pieces[piece]);
    }
    return count;
}

static int calculate_dynamic_depth(Board *board, const SearchLimits *limits) {
    int depth = 3;  /* Base depth. */

    /* +1 if in check. */
    if (board_is_in_check(board, board->side)) {
        depth++;
    }

    /* Count pieces on the board. */
    int piece_count = count_pieces(board);

    /* +1 if less than 15 pieces. */
    if (piece_count < 15) {
        depth++;
    }

    /* +1 again if less than 10 pieces. */
    if (piece_count < 10) {
        depth++;
    }

    /* -1 if we have less than a minute on the clock. */
    if (limits != NULL) {
        int current_time = (board->side == WHITE) ? limits->wtime_ms : limits->btime_ms;
        if (current_time > 0 && current_time < 60000) {
            depth--;
        }
    }

    /* Ensure depth is at least 1. */
    if (depth < 1) {
        depth = 1;
    }

    return depth;
}

static float evaluate(Board *board) {
    if (board == NULL) {
        return 0.0f;
    }

    /* Check terminal conditions first. */
    MoveList list;
    movegen_generate_legal(board, &list);

    if (list.count == 0) {
        /* No legal moves: checkmate or stalemate. */
        if (board_is_in_check(board, board->side)) {
            /* Checkmate: very bad for side to move. */
            return -10000.0f;
        } else {
            /* Stalemate: draw. */
            return 0.0f;
        }
    }

    /* 50-move rule: draw. */
    if (board->halfmove_clock >= 100) {
        return 0.0f;
    }

    /* Material evaluation. */
    float score = 0.0f;
    int side_to_move = board->side;

    for (int piece = 0; piece < PIECE_NB; ++piece) {
        int value = piece_values[piece % 6];
        int count = popcount_u64(board->pieces[piece]);
        int piece_side = (piece < BLACK_PAWN) ? WHITE : BLACK;

        if (piece_side == side_to_move) {
            score += (float)(value * count);
        } else {
            score -= (float)(value * count);
        }
    }

    return score;
}

/* Estimate move score for move ordering. */
static int estimate_move_score(Board *board, Move move) {
    int score = 0;

    /* Captures - MVV/LVA (Most Valuable Victim / Least Valuable Aggressor). */
    if (move_iscapture(board, move)) {
        int captured_piece = board_piece_at(board, move_to(move));
        int moving_piece = board_piece_at(board, move_from(move));
        
        if (captured_piece >= 0 && moving_piece >= 0) {
            int captured_value = piece_values[captured_piece % 6];
            int moving_value = piece_values[moving_piece % 6];
            int capture_value = captured_value - (moving_value / 100);
            score += 10000 + capture_value;
        }
    }

    /* Promotions. */
    if (move_promotion(move) != MOVE_PROMO_NONE) {
        score += 9000 + (move_promotion(move) * 100);
    }

    /* Castling. */
    if ((move_flags(move) & MOVE_FLAG_CASTLE) != 0) {
        score += 1000;
    }

    /* Check if move gives check. */
    Undo undo;
    if (board_make_move(board, move, &undo)) {
        if (board_is_in_check(board, board->side)) {
            score += 500;
        }
        board_unmake_move(board, &undo);
    }

    return score;
}

/* Compare function for qsort - sort in descending order of score. */
static int compare_scored_moves(const void *a, const void *b) {
    const ScoredMove *move_a = (const ScoredMove *)a;
    const ScoredMove *move_b = (const ScoredMove *)b;
    return move_b->score - move_a->score;
}

/* Quiescence search: only explores captures and checks. */
static float quiescence(Board *board, float alpha, float beta) {
    /* Stand-pat: evaluate current position. */
    float stand_pat = evaluate(board);

    if (stand_pat >= beta) {
        return beta;
    }

    if (stand_pat > alpha) {
        alpha = stand_pat;
    }

    /* Generate legal moves. */
    MoveList list;
    movegen_generate_legal(board, &list);

    /* Build and sort list of captures and checks. */
    ScoredMove scored_moves[256];
    int scored_count = 0;
    for (int i = 0; i < list.count; ++i) {
        Move move = list.moves[i];
        if (move_iscapture(board, move) || move_ischeck(board, move)) {
            scored_moves[scored_count].move = move;
            scored_moves[scored_count].score = estimate_move_score(board, move);
            ++scored_count;
        }
    }

    /* Sort moves by estimated score. */
    qsort(scored_moves, scored_count, sizeof(ScoredMove), compare_scored_moves);

    /* Search only captures and checks. */
    for (int i = 0; i < scored_count; ++i) {
        Move move = scored_moves[i].move;
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        float score = -quiescence(board, -beta, -alpha);

        board_unmake_move(board, &undo);

        if (score >= beta) {
            return beta;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    return alpha;
}

/* Alpha-beta pruned negamax search. */
static SearchResult negamax(Board *board, int depth, float alpha, float beta) {
    SearchResult result = {0.0f, MOVE_NONE};

    /* Leaf node: run quiescence search. */
    if (depth == 0) {
        result.score = quiescence(board, alpha, beta);
        return result;
    }

    /* Generate legal moves. */
    MoveList list;
    movegen_generate_legal(board, &list);

    /* No moves: run quiescence search on terminal position. */
    if (list.count == 0) {
        result.score = quiescence(board, alpha, beta);
        return result;
    }

    /* Build and sort move list. */
    ScoredMove scored_moves[256];
    for (int i = 0; i < list.count; ++i) {
        scored_moves[i].move = list.moves[i];
        scored_moves[i].score = estimate_move_score(board, list.moves[i]);
    }
    qsort(scored_moves, list.count, sizeof(ScoredMove), compare_scored_moves);

    /* Search moves with alpha-beta pruning. */
    for (int i = 0; i < list.count; ++i) {
        Move move = scored_moves[i].move;
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        /* Recurse with negated alpha and beta. */
        SearchResult child = negamax(board, depth - 1, -beta, -alpha);
        float score = -child.score;

        board_unmake_move(board, &undo);

        /* Update best move and score. */
        if (score > result.score || result.move == MOVE_NONE) {
            result.score = score;
            result.move = move;
        }

        /* Update alpha. */
        if (score > alpha) {
            alpha = score;
        }

        /* Beta cutoff: prune remaining siblings. */
        if (alpha >= beta) {
            break;
        }
    }

    return result;
}

static SearchResult search_root(Board *board, int depth) {
    SearchResult result = {0.0f, MOVE_NONE};
    float alpha = -FLT_MAX;
    float beta = FLT_MAX;

    MoveList list;
    movegen_generate_legal(board, &list);

    if (list.count == 0) {
        result.score = quiescence(board, alpha, beta);
        return result;
    }

    ScoredMove scored_moves[256];
    for (int i = 0; i < list.count; ++i) {
        scored_moves[i].move = list.moves[i];
        scored_moves[i].score = estimate_move_score(board, list.moves[i]);
    }
    qsort(scored_moves, list.count, sizeof(ScoredMove), compare_scored_moves);

    for (int i = 0; i < list.count; ++i) {
        Move move = scored_moves[i].move;
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha);
        float score = -child.score;

        board_unmake_move(board, &undo);

        print_move_info(depth, i + 1, move, score);

        if (score > result.score || result.move == MOVE_NONE) {
            result.score = score;
            result.move = move;
        }

        if (score > alpha) {
            alpha = score;
        }

        if (alpha >= beta) {
            break;
        }
    }

    return result;
}

Move think(Board *board, const SearchLimits *limits) {
    if (board == NULL) {
        return MOVE_NONE;
    }

    int depth = 0;
    if (limits != NULL && limits->depth > 0) {
        /* Explicit depth set via UCI. */
        depth = limits->depth;
    } else {
        /* Depth not explicitly set, calculate dynamically. */
        depth = calculate_dynamic_depth(board, limits);
    }

    SearchResult result = {0.0f, MOVE_NONE};
    for (int current_depth = 1; current_depth <= depth; ++current_depth) {
        result = search_root(board, current_depth);
        print_depth_info(current_depth, result);
    }

    if (result.move == MOVE_NONE) {
        return MOVE_NONE;
    }

    return result.move;
}