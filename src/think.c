#include "think.h"
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

Move think(Board *board, const SearchLimits *limits) {
    (void)limits;

    if (board == NULL) {
        return MOVE_NONE;
    }

    SearchResult result = negamax(board, 3, -FLT_MAX, FLT_MAX);
    if (result.move == MOVE_NONE) {
        return MOVE_NONE;
    }

    return result.move;
}