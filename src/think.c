#include "think.h"
#include <stdio.h>
#include "float.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>

#define MAX_PV_MOVES 128
#define TRANSITION_TABLE_SIZE 1 << 20 /* 1 million entries */
#define MAX_ORDERED_MOVES 256

typedef struct {
    float score;
    Move move;
    Move pv[MAX_PV_MOVES];
    int pv_length;
} SearchResult;

typedef struct {
    Move move;
    int score;
} ScoredMove;

typedef struct {
    Move move;
    float score;
    bool searched;
} RankedMove;

typedef struct {
    bool valid;
    U64 hash;
    int depth;
    int move_count;
    Move moves[MAX_ORDERED_MOVES];
} TransitionEntry;

typedef struct {
    TransitionEntry *entries;
    size_t size;
} TransitionTable;

typedef struct {
    unsigned long long nodes;
    int seldepth;
} SearchStats;

static long long current_time_ms(void);

typedef struct {
    bool time_limited;
    long long stop_time_ms;
    bool stop;
} SearchControl;

static bool search_should_stop(SearchControl *control) {
    if (control == NULL) {
        return false;
    }

    if (control->stop) {
        return true;
    }

    if (!control->time_limited) {
        return false;
    }

    if (current_time_ms() >= control->stop_time_ms) {
        control->stop = true;
        return true;
    }

    return false;
}

static int estimate_move_score(Board *board, Move move);

/* File masks - one per file (A-H) */
static const U64 file_masks[8] = {
    0x0101010101010101ULL, /* A-file */
    0x0202020202020202ULL, /* B-file */
    0x0404040404040404ULL, /* C-file */
    0x0808080808080808ULL, /* D-file */
    0x1010101010101010ULL, /* E-file */
    0x2020202020202020ULL, /* F-file */
    0x4040404040404040ULL, /* G-file */
    0x8080808080808080ULL   /* H-file */
};

/* Rank masks - one per rank (1-8) */
static const U64 rank_masks[8] = {
    0x00000000000000FFULL, /* Rank 1 */
    0x000000000000FF00ULL, /* Rank 2 */
    0x0000000000FF0000ULL, /* Rank 3 */
    0x00000000FF000000ULL, /* Rank 4 */
    0x000000FF00000000ULL, /* Rank 5 */
    0x0000FF0000000000ULL, /* Rank 6 */
    0x00FF000000000000ULL, /* Rank 7 */
    0xFF00000000000000ULL   /* Rank 8 */
};

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

static long long current_time_ms(void) {
    clock_t now = clock();
    if (now < 0) {
        return 0;
    }

    return (long long)((double)now * 1000.0 / (double)CLOCKS_PER_SEC);
}

static unsigned long long compute_nps(unsigned long long nodes, long long elapsed_ms) {
    if (elapsed_ms <= 0) {
        return nodes * 1000ULL;
    }

    return (nodes * 1000ULL) / (unsigned long long)elapsed_ms;
}

static void print_depth_info(int depth, const SearchResult *result, const SearchStats *stats, long long elapsed_ms) {
    unsigned long long nps = compute_nps(stats->nodes, elapsed_ms);
    printf("info depth %d seldepth %d score cp %d nodes %llu nps %llu time %lld",
           depth,
           stats->seldepth,
           score_to_cp(result->score),
           stats->nodes,
           nps,
           elapsed_ms);

    if (result->pv_length > 0) {
        printf(" pv");
        for (int i = 0; i < result->pv_length; ++i) {
            char move_buffer[6];
            move_to_string(result->pv[i], move_buffer);
            printf(" %s", move_buffer);
        }
    }

    printf("\n");
    fflush(stdout);
}

static const int piece_values[6] = {
    100, /* Pawn */
    300, /* Knight */
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

static int rank_of(int square) {
    return square >> 3;
}

static int file_of(int square) {
    return square & 7;
}

static int count_attackers_on_square(const Board *board, int square, int attacker_side) {
    int attackers = 0;

    int file = file_of(square);
    int rank = rank_of(square);

    if (attacker_side == WHITE) {
        if (file > 0 && rank > 0 && (board->pieces[WHITE_PAWN] & (1ULL << (square - 9)))) {
            ++attackers;
        }
        if (file < 7 && rank > 0 && (board->pieces[WHITE_PAWN] & (1ULL << (square - 7)))) {
            ++attackers;
        }
    } else {
        if (file > 0 && rank < 7 && (board->pieces[BLACK_PAWN] & (1ULL << (square + 7)))) {
            ++attackers;
        }
        if (file < 7 && rank < 7 && (board->pieces[BLACK_PAWN] & (1ULL << (square + 9)))) {
            ++attackers;
        }
    }

    U64 knights = (attacker_side == WHITE) ? board->pieces[WHITE_KNIGHT] : board->pieces[BLACK_KNIGHT];
    attackers += popcount_u64(bitboard_knight_attacks(square) & knights);

    U64 kings = (attacker_side == WHITE) ? board->pieces[WHITE_KING] : board->pieces[BLACK_KING];
    attackers += popcount_u64(bitboard_king_attacks(square) & kings);

    U64 bishops_and_queens = (attacker_side == WHITE)
                                 ? (board->pieces[WHITE_BISHOP] | board->pieces[WHITE_QUEEN])
                                 : (board->pieces[BLACK_BISHOP] | board->pieces[BLACK_QUEEN]);
    attackers += popcount_u64(bitboard_bishop_attacks(square, board->occupancy[BOTH]) & bishops_and_queens);

    U64 rooks_and_queens = (attacker_side == WHITE)
                               ? (board->pieces[WHITE_ROOK] | board->pieces[WHITE_QUEEN])
                               : (board->pieces[BLACK_ROOK] | board->pieces[BLACK_QUEEN]);
    attackers += popcount_u64(bitboard_rook_attacks(square, board->occupancy[BOTH]) & rooks_and_queens);

    return attackers;
}

static void count_pawns_per_file(U64 pawns, int pawns_per_file[8]) {
    for (int i = 0; i < 8; ++i) {
        pawns_per_file[i] = 0;
    }

    for (int f = 0; f < 8; ++f) {
    pawns_per_file[f] = popcount_u64(pawns & file_masks[f]);
    }
}

/* Get mask for all ranks ahead of given rank (for white pawns: rank+1 to 7) */
static U64 get_ranks_ahead_white(int rank) {
    if (rank >= 7) return 0;
    return 0xFFFFFFFFFFFFFFFFULL << ((rank + 1) * 8);
}

/* Get mask for all ranks ahead of given rank (for black pawns: 0 to rank-1) */
static U64 get_ranks_ahead_black(int rank) {
    if (rank <= 0) return 0;
    return (1ULL << (rank * 8)) - 1;
}

static void mark_passed_pawns(const Board *board, int side, bool passed_pawns[64]) {
    for (int i = 0; i < 64; ++i) {
        passed_pawns[i] = false;
    }

    U64 own_pawns = board->pieces[side == WHITE ? WHITE_PAWN : BLACK_PAWN];
    U64 enemy_pawns = board->pieces[side == WHITE ? BLACK_PAWN : WHITE_PAWN];

    U64 bb = own_pawns;
    while (bb) {
        int square = bitboard_pop_lsb(&bb);
        int file = file_of(square);
        int rank = rank_of(square);
        
        /* Create mask for current file and adjacent files */
        U64 check_files = 0;
        if (file > 0) check_files |= file_masks[file - 1];
        check_files |= file_masks[file];
        if (file < 7) check_files |= file_masks[file + 1];
        
        /* Get mask for ranks ahead of this pawn */
        U64 ranks_ahead = (side == WHITE) ? get_ranks_ahead_white(rank) : get_ranks_ahead_black(rank);
        
        /* Check if any enemy pawns exist in the check region */
        if ((enemy_pawns & check_files & ranks_ahead) == 0) {
            passed_pawns[square] = true;
        }
    }
}

static float evaluate_piece(const Board *board,
                            int piece,
                            int square,
                            int endgame,
                            const bool passed_pawns[64],
                            const int white_pawns_per_file[8],
                            const int black_pawns_per_file[8]) {
    int side = board_piece_color(piece);
    int type = board_piece_type(piece);
    int file = file_of(square);
    int rank = rank_of(square);
    bool is_white = (side == WHITE);
    float piece_value = (float)piece_values[type];

    if (type == WHITE_PAWN) {
        int pawn_rank = is_white ? rank : (7 - rank);
        if (endgame == 1) { //Reward advanced pawns in the endgame
            piece_value += 2.0f * (float)pawn_rank;
            if (passed_pawns[square]) {
                //Passed pawns are further rewarded for advancement
                piece_value += 4.0f * (float)pawn_rank;
            }
        }

        const int *pawns_per_file = is_white ? white_pawns_per_file : black_pawns_per_file;
        int this_file_count = pawns_per_file[file];

        if (this_file_count > 1) {
            //Doubled pawn penalty: -5 points for each same colour pawn on the file
            piece_value -= 5.0f * (float)(this_file_count - 1);
        }

        bool has_left = (file > 0) && (pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right) {
            //Isolated pawn penalty: -10 points if no same colour pawns on adjacent files
            piece_value -= 10.0f;
        }

        return piece_value;
    }

    if (type == WHITE_KNIGHT) {
        //Knights on the rim are grim
        return piece_value + 3.0f * (float)popcount_u64(bitboard_knight_attacks(square));
    }

    if (type == WHITE_BISHOP) {
        //Slider attacks not blocked by pawns, so this rewards bishops with more mobility
        U64 pawn_occupancy = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
        return piece_value + 1.5f * (float)popcount_u64(bitboard_bishop_attacks(square, pawn_occupancy));
    }

    if (type == WHITE_ROOK) {
        if (endgame == -1) {//None endgame evaluation
            //Reward for squares controlled
            piece_value += 1.0f * (float)popcount_u64(bitboard_rook_attacks(square, board->occupancy[BOTH]));

            U64 all_pawns = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
            U64 file_mask = file_masks[file];
            //Open file bonus: +50 points if no pawns on the file
            if (popcount_u64(all_pawns & file_mask) == 0) {
                piece_value += 50.0f;
            }
        } else {
            //Rooks are better in the endgame
            piece_value += 100.0f;
        }

        return piece_value;
    }

    if (type == WHITE_QUEEN) {
        return piece_value + 0.5f * (float)popcount_u64(bitboard_queen_attacks(square, board->occupancy[BOTH]));
    }
    // If nothing prior it is a king
    if (endgame == -1) {
        //In the opening/midgame, king safety is important. Penalise kings with more possible attacks against them.
        piece_value -= (float)popcount_u64(bitboard_queen_attacks(square, board->occupancy[BOTH]));
    }
    //Calculate Manhattan distance to closest corner
    int distance_a1 = file + rank;
    int distance_h1 = (7 - file) + rank;
    int distance_a8 = file + (7 - rank);
    int distance_h8 = (7 - file) + (7 - rank);
    int corner_distance = distance_a1;
    if (distance_h1 < corner_distance) {
        corner_distance = distance_h1;
    }
    if (distance_a8 < corner_distance) {
        corner_distance = distance_a8;
    }
    if (distance_h8 < corner_distance) {
        corner_distance = distance_h8;
    }

    //If endgame then favour activity over safety
    //Penalty in the opening/middlegame becomes reward in the endgame
    piece_value += (float)(endgame * corner_distance * 3);
    return piece_value;
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

static bool is_endgame_position(const Board *board) {
    if (board == NULL) {
        return false;
    }

    int non_king_count = 0;
    non_king_count += popcount_u64(board->pieces[WHITE_KNIGHT]);
    non_king_count += popcount_u64(board->pieces[WHITE_BISHOP]);
    non_king_count += popcount_u64(board->pieces[WHITE_ROOK]);
    non_king_count += popcount_u64(board->pieces[WHITE_QUEEN]);
    non_king_count += popcount_u64(board->pieces[BLACK_KNIGHT]);
    non_king_count += popcount_u64(board->pieces[BLACK_BISHOP]);
    non_king_count += popcount_u64(board->pieces[BLACK_ROOK]);
    non_king_count += popcount_u64(board->pieces[BLACK_QUEEN]);

    return non_king_count <= 4;
}

static float evaluate(Board *board, const RepetitionHistory *history) {
    if (board == NULL) {
        return 0.0f;
    }

    if (board_is_draw(board, history)) {
        return 0.0f;
    }

    MoveList list;
    movegen_generate_legal(board, &list);

    if (list.count == 0) {
        /* No legal moves: checkmate or stalemate. */
        if (board_is_in_check(board, board->side)) {
            /* Checkmate: very bad for side to move. */
            return -100000.0f;
        }
    }

    int side_to_move = board->side;

    int endgame = is_endgame_position(board) ? 1 : -1;

    int white_pawns_per_file[8];
    int black_pawns_per_file[8];
    count_pawns_per_file(board->pieces[WHITE_PAWN], white_pawns_per_file);
    count_pawns_per_file(board->pieces[BLACK_PAWN], black_pawns_per_file);

    bool white_passed_pawns[64];
    bool black_passed_pawns[64];
    mark_passed_pawns(board, WHITE, white_passed_pawns);
    mark_passed_pawns(board, BLACK, black_passed_pawns);

    float white_score = 0.0f;
    float black_score = 0.0f;

    for (int piece = 0; piece < PIECE_NB; ++piece) {
        U64 bb = board->pieces[piece];
        while (bb) {
            int square = bitboard_pop_lsb(&bb);
            int side = board_piece_color(piece);
            const bool *passed = (side == WHITE) ? white_passed_pawns : black_passed_pawns;
            float value = evaluate_piece(board,
                                         piece,
                                         square,
                                         endgame,
                                         passed,
                                         white_pawns_per_file,
                                         black_pawns_per_file);

            if (side == WHITE) {
                white_score += value;
            } else {
                black_score += value;
            }
        }
    }

    static const int center_squares[4] = {27, 28, 35, 36}; /* d4, e4, d5, e5 */
    for (int i = 0; i < 4; ++i) {
        int square = center_squares[i];
        white_score += 3.0f*(float)count_attackers_on_square(board, square, WHITE);
        black_score += 3.0f*(float)count_attackers_on_square(board, square, BLACK);
    }

    //Unless the position is zugzwang, having a move will make the position better
    float tempo_bonus = (endgame == -1) ? 10.0f : 0.0f;

    if (side_to_move == WHITE) {
        return white_score - black_score + tempo_bonus;
    }

    return black_score - white_score + tempo_bonus;
}

/* Estimate move score for move ordering. This is intentionally cheap. */
static int estimate_move_score(Board *board, Move move) {
    int score = 0;
    int flags = move_flags(move);

    /* Captures - MVV/LVA (Most Valuable Victim / Least Valuable Aggressor). */
    if (move_iscapture(board, move)) {
        int attacker_piece = board_piece_at(board, move_from(move));
        int attacker_type = board_piece_type(attacker_piece);

        int victim_piece = board_piece_at(board, move_to(move));
        int victim_type;

        if ((flags & MOVE_FLAG_EN_PASSANT) != 0) {
            victim_type = WHITE_PAWN; /* type 0 */
        } else {
            victim_type = board_piece_type(victim_piece);
        }

        if (attacker_type >= 0 && attacker_type < 6 && victim_type >= 0 && victim_type < 6) {
            score += 10000 + piece_values[victim_type] * 10 - piece_values[attacker_type];
        }
    }

    /* Promotions. */
    if (move_promotion(move) != MOVE_PROMO_NONE) {
        static const int promo_bonus[5] = {0, 300, 320, 500, 950};
        int promo = move_promotion(move);
        if (promo >= 0 && promo <= 4) {
            score += 20000 + promo_bonus[promo];
        }
    }

    /* Castling. */
    if ((flags & MOVE_FLAG_CASTLE) != 0) {
        score += 100;
    }

    /* Checks (+). I saw online that this is too much computation to be worth considering for move ordering
    Keep the code in case we change our mind
    Undo undo;
    if (board_make_move(board, move, &undo)) {
        if (board_is_in_check(board, board->side)) {
            score += 500;
        }
        board_unmake_move(board, &undo);
    } */

    return score;
}

/* Compare function for qsort - sort in descending order of score. */
static int compare_scored_moves(const void *a, const void *b) {
    const ScoredMove *move_a = (const ScoredMove *)a;
    const ScoredMove *move_b = (const ScoredMove *)b;
    return move_b->score - move_a->score;
}

static int compare_ranked_moves(const void *a, const void *b) {
    const RankedMove *move_a = (const RankedMove *)a;
    const RankedMove *move_b = (const RankedMove *)b;

    if (move_a->score < move_b->score) {
        return 1;
    }

    if (move_a->score > move_b->score) {
        return -1;
    }

    return 0;
}

static size_t transition_table_index(const TransitionTable *table, U64 hash) {
    return (size_t)(hash & (table->size - 1U));
}

static const TransitionEntry *transition_table_lookup(const TransitionTable *table, U64 hash) {
    if (table == NULL || table->entries == NULL || table->size == 0) {
        return NULL;
    }

    const TransitionEntry *entry = &table->entries[transition_table_index(table, hash)];
    if (!entry->valid || entry->hash != hash) {
        return NULL;
    }

    return entry;
}

static void transition_table_store(TransitionTable *table, U64 hash, int depth, const Move *moves, int move_count) {
    if (table == NULL || table->entries == NULL || table->size == 0 || moves == NULL || move_count <= 0) {
        return;
    }

    if (move_count > MAX_ORDERED_MOVES) {
        move_count = MAX_ORDERED_MOVES;
    }

    TransitionEntry *entry = &table->entries[transition_table_index(table, hash)];
    if (entry->valid && entry->hash == hash && depth <= entry->depth) {
        return;
    }

    if (entry->valid && entry->hash != hash && depth <= entry->depth) {
        return;
    }

    entry->valid = true;
    entry->hash = hash;
    entry->depth = depth;
    entry->move_count = move_count;
    memcpy(entry->moves, moves, (size_t)move_count * sizeof(Move));
}

static int find_move_index(const MoveList *list, Move move) {
    if (list == NULL) {
        return -1;
    }

    for (int i = 0; i < list->count; ++i) {
        if (list->moves[i] == move) {
            return i;
        }
    }

    return -1;
}

static int build_ordered_moves(Board *board,
                               const MoveList *list,
                               const TransitionTable *table,
                               Move ordered_moves[MAX_ORDERED_MOVES]) {
    if (board == NULL || list == NULL || ordered_moves == NULL || list->count <= 0) {
        return 0;
    }

    U64 hash = board_position_key(board);
    const TransitionEntry *entry = transition_table_lookup(table, hash);

    if (entry == NULL) {
        ScoredMove scored_moves[MAX_ORDERED_MOVES];
        int scored_count = 0;
        for (int i = 0; i < list->count && i < MAX_ORDERED_MOVES; ++i) {
            scored_moves[scored_count].move = list->moves[i];
            scored_moves[scored_count].score = estimate_move_score(board, list->moves[i]);
            ++scored_count;
        }

        qsort(scored_moves, (size_t)scored_count, sizeof(ScoredMove), compare_scored_moves);
        for (int i = 0; i < scored_count; ++i) {
            ordered_moves[i] = scored_moves[i].move;
        }

        return scored_count;
    }

    bool used[MAX_ORDERED_MOVES] = {false};
    int ordered_count = 0;

    for (int i = 0; i < entry->move_count && ordered_count < list->count; ++i) {
        Move move = entry->moves[i];
        int index = find_move_index(list, move);
        if (index >= 0 && !used[index]) {
            ordered_moves[ordered_count++] = move;
            used[index] = true;
        }
    }

    ScoredMove fallback_moves[MAX_ORDERED_MOVES];
    int fallback_count = 0;
    for (int i = 0; i < list->count && fallback_count < MAX_ORDERED_MOVES; ++i) {
        if (used[i]) {
            continue;
        }

        fallback_moves[fallback_count].move = list->moves[i];
        fallback_moves[fallback_count].score = estimate_move_score(board, list->moves[i]);
        ++fallback_count;
    }

    qsort(fallback_moves, (size_t)fallback_count, sizeof(ScoredMove), compare_scored_moves);
    for (int i = 0; i < fallback_count && ordered_count < MAX_ORDERED_MOVES; ++i) {
        ordered_moves[ordered_count++] = fallback_moves[i].move;
    }

    return ordered_count;
}

static int finalize_move_order(const RankedMove *ranked_moves, int move_count, Move final_order[MAX_ORDERED_MOVES]) {
    if (ranked_moves == NULL || final_order == NULL || move_count <= 0) {
        return 0;
    }

    RankedMove searched_moves[MAX_ORDERED_MOVES];
    int searched_count = 0;

    for (int i = 0; i < move_count && i < MAX_ORDERED_MOVES; ++i) {
        if (ranked_moves[i].searched) {
            searched_moves[searched_count++] = ranked_moves[i];
        }
    }

    qsort(searched_moves, (size_t)searched_count, sizeof(RankedMove), compare_ranked_moves);

    int final_count = 0;
    for (int i = 0; i < searched_count && final_count < MAX_ORDERED_MOVES; ++i) {
        final_order[final_count++] = searched_moves[i].move;
    }

    for (int i = 0; i < move_count && final_count < MAX_ORDERED_MOVES; ++i) {
        if (!ranked_moves[i].searched) {
            final_order[final_count++] = ranked_moves[i].move;
        }
    }

    return final_count;
}

/* Quiescence search: only explores captures and checks. */
static float quiescence(Board *board,
                        float alpha,
                        float beta,
                        RepetitionHistory *history,
                        SearchStats *stats,
                        int ply,
                        TransitionTable *table,
                        SearchControl *control) {
    if (search_should_stop(control)) {
        return evaluate(board, history);
    }

    ++stats->nodes;
    if (ply > stats->seldepth) {
        stats->seldepth = ply;
    }

    if (board_is_draw(board, history)) {
        return 0.0f;
    }

    /* Stand-pat: evaluate current position. */
    float stand_pat = evaluate(board, history);

    if (stand_pat >= beta) {
        return beta;
    }

    if (stand_pat > alpha) {
        alpha = stand_pat;
    }

    /* Generate legal moves. */
    MoveList list;
    movegen_generate_legal(board, &list);

    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = build_ordered_moves(board, &list, table, ordered_moves);

    RankedMove ranked_moves[MAX_ORDERED_MOVES] = {0};
    for (int i = 0; i < ordered_count; ++i) {
        ranked_moves[i].move = ordered_moves[i];
        ranked_moves[i].searched = false;
    }

    // Only consider captures and checks in quiescence search.
    for (int i = 0; i < ordered_count; ++i) {
        if (search_should_stop(control)) {
            break;
        }

        Move move = ranked_moves[i].move;
        if (!move_iscapture(board, move) && !move_ischeck(board, move)) {
            continue;
        }

        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        float score = -quiescence(board, -beta, -alpha, history, stats, ply + 1, table, control);

        ranked_moves[i].score = score;
        ranked_moves[i].searched = true;

        --history->count;

        board_unmake_move(board, &undo);

        if (score >= beta) {
            return beta;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    Move final_order[MAX_ORDERED_MOVES];
    int final_count = finalize_move_order(ranked_moves, ordered_count, final_order);
    if (final_count > 0) {
        transition_table_store(table, board_position_key(board), 0, final_order, final_count);
    }

    return alpha;
}

/* Alpha-beta pruned negamax search. */
static SearchResult negamax(Board *board,
                            int depth,
                            float alpha,
                            float beta,
                            RepetitionHistory *history,
                            SearchStats *stats,
                            int ply,
                            TransitionTable *table,
                            SearchControl *control) {
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0};

    if (search_should_stop(control)) {
        result.score = evaluate(board, history);
        return result;
    }

    ++stats->nodes;

    if (board_is_draw(board, history)) {
        result.score = 0.0f;
        return result;
    }

    /* Null-move pruning, avoided in endgames to avoid zugzwang issues. */
    if (depth >= 3 &&
        beta < 10000.0f &&
        !board_is_in_check(board, board->side) &&
        !is_endgame_position(board)) {
        const int reduction = 2;
        Undo undo;
        undo.snapshot = *board;

        if (board->side == BLACK) {
            ++board->fullmove_number;
        }
        board->side ^= 1;
        board->ep_square = -1;
        ++board->halfmove_clock;

        SearchResult null_child = negamax(board,
                          depth - 1 - reduction,
                          -beta,
                          -beta + 1.0f,
                          history,
                          stats,
                          ply + 1,
                          table,
                          control);
        float null_score = -null_child.score;

        board_unmake_move(board, &undo);

        if (null_score >= beta) {
            result.score = beta;
            return result;
        }
    }

    /* Leaf node: run quiescence search. */
    if (depth == 0) {
        result.score = quiescence(board, alpha, beta, history, stats, ply, table, control);
        return result;
    }

    /* Generate legal moves. */
    MoveList list;
    movegen_generate_legal(board, &list);

    /* No moves: run quiescence search on terminal position. */
    if (list.count == 0) {
        result.score = quiescence(board, alpha, beta, history, stats, ply, table, control);
        return result;
    }

    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = build_ordered_moves(board, &list, table, ordered_moves);

    RankedMove ranked_moves[MAX_ORDERED_MOVES] = {0};
    for (int i = 0; i < ordered_count; ++i) {
        ranked_moves[i].move = ordered_moves[i];
        ranked_moves[i].searched = false;
    }

    /* Search moves with alpha-beta pruning. */
    for (int i = 0; i < ordered_count; ++i) {
        if (search_should_stop(control)) {
            break;
        }

        Move move = ordered_moves[i];
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        /* Recurse with negated alpha and beta. */
        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha, history, stats, ply + 1, table, control);
        float score = -child.score;

        ranked_moves[i].score = score;
        ranked_moves[i].searched = true;

        --history->count;

        board_unmake_move(board, &undo);

        /* Update best move and score. */
        if (score > result.score || result.move == MOVE_NONE) {
            result.score = score;
            result.move = move;
            result.pv[0] = move;
            result.pv_length = 1;
            for (int j = 0; j < child.pv_length && result.pv_length < MAX_PV_MOVES; ++j) {
                result.pv[result.pv_length++] = child.pv[j];
            }
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

    Move final_order[MAX_ORDERED_MOVES];
    int final_count = finalize_move_order(ranked_moves, ordered_count, final_order);
    if (final_count > 0) {
        transition_table_store(table, board_position_key(board), depth, final_order, final_count);
    }

    return result;
}

static SearchResult search_root(Board *board,
                                int depth,
                                RepetitionHistory *history,
                                SearchStats *stats,
                                TransitionTable *table,
                                SearchControl *control) {
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0};
    float alpha = -FLT_MAX;
    float beta = FLT_MAX;

    MoveList list;
    movegen_generate_legal(board, &list);

    if (board_is_draw(board, history)) {
        result.score = 0.0f;
        if (list.count > 0) {
            result.move = list.moves[0];
            result.pv[0] = list.moves[0];
            result.pv_length = 1;
        }
        return result;
    }

    if (list.count == 0) {
        result.score = quiescence(board, alpha, beta, history, stats, 0, table, control);
        return result;
    }

    Move ordered_moves[MAX_ORDERED_MOVES];
    int ordered_count = build_ordered_moves(board, &list, table, ordered_moves);

    RankedMove ranked_moves[MAX_ORDERED_MOVES] = {0};
    for (int i = 0; i < ordered_count; ++i) {
        ranked_moves[i].move = ordered_moves[i];
        ranked_moves[i].searched = false;
    }

    for (int i = 0; i < ordered_count; ++i) {
        if (search_should_stop(control)) {
            break;
        }

        Move move = ordered_moves[i];
        Undo undo;

        if (!board_make_move(board, move, &undo)) {
            continue;
        }

        U64 key = board_position_key(board);
        if (!repetition_history_push(history, key)) {
            board_unmake_move(board, &undo);
            continue;
        }

        SearchResult child = negamax(board, depth - 1, -beta, -alpha, history, stats, 1, table, control);
        float score = -child.score;

        ranked_moves[i].score = score;
        ranked_moves[i].searched = true;

        --history->count;

        board_unmake_move(board, &undo);

        print_move_info(depth, i + 1, move, score);

        if (score > result.score || result.move == MOVE_NONE) {
            result.score = score;
            result.move = move;
            result.pv[0] = move;
            result.pv_length = 1;
            for (int j = 0; j < child.pv_length && result.pv_length < MAX_PV_MOVES; ++j) {
                result.pv[result.pv_length++] = child.pv[j];
            }
        }

        if (score > alpha) {
            alpha = score;
        }

        if (alpha >= beta) {
            break;
        }
    }

    Move final_order[MAX_ORDERED_MOVES];
    int final_count = finalize_move_order(ranked_moves, ordered_count, final_order);
    if (final_count > 0) {
        transition_table_store(table, board_position_key(board), depth, final_order, final_count);
    }

    return result;
}

Move think(Board *board, const SearchLimits *limits, const SearchOptions *options, const RepetitionHistory *history) {
    if (board == NULL) {
        return MOVE_NONE;
    }   

    int target_depth = 4;
    int movetime_ms = 0;
    int overhead_ms = 50;
    bool depth_explicitly_set = false;
    const int max_iterative_depth = 64;

    if (options != NULL) {
        overhead_ms = options->overhead_ms;
    }

    if (limits != NULL) {
        if (limits->depth > 0) {
            target_depth = limits->depth;
            depth_explicitly_set = true;
        }
        if (limits->movetime_ms > 0) {
            movetime_ms = limits->movetime_ms;
        }
    }

    if (movetime_ms > 0 && !depth_explicitly_set) {
        /* In pure movetime mode, deepen until the time budget is consumed. */
        target_depth = max_iterative_depth;
    }

    RepetitionHistory search_history;
    repetition_history_init(&search_history);
    if (history != NULL) {
        search_history = *history;
    }

    long long start_time_ms = current_time_ms();
    int time_to_use_ms = movetime_ms - overhead_ms;
    if (time_to_use_ms < 10) {
        time_to_use_ms = 10;
    }

    SearchControl control = {0};
    if (movetime_ms > 0) {
        control.time_limited = true;
        control.stop_time_ms = start_time_ms + (long long)time_to_use_ms;
    }

    TransitionTable table = {0};
    table.size = TRANSITION_TABLE_SIZE;
    table.entries = calloc(table.size, sizeof(*table.entries));

    SearchResult best_result = {0.0f, MOVE_NONE, {0}, 0};
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0};

    /* Iterative deepening: search depths 1 through target_depth. */
    for (int depth = 1; depth <= target_depth; ++depth) {
        if (movetime_ms > 0) {
            long long elapsed_before_depth_ms = current_time_ms() - start_time_ms;
            if (elapsed_before_depth_ms < 0) {
                elapsed_before_depth_ms = 0;
            }

            if (elapsed_before_depth_ms >= time_to_use_ms && best_result.move != MOVE_NONE) {
                break;
            }
        }

        SearchStats stats = {0ULL, depth};
        control.stop = false;

        result = search_root(board,
                             depth,
                             &search_history,
                             &stats,
                             table.entries != NULL ? &table : NULL,
                             &control);

        if (control.stop) {
            break;
        }

        long long elapsed_ms = current_time_ms() - start_time_ms;
        if (elapsed_ms < 0) {
            elapsed_ms = 0;
        }

        print_depth_info(depth, &result, &stats, elapsed_ms);

        /* Update best result if we have a valid move */
        if (result.move != MOVE_NONE) {
            best_result = result;
        }

        /* Check if we should stop searching */
        if (movetime_ms > 0 && elapsed_ms >= time_to_use_ms) {
            break;
        }
    }

    if (best_result.move == MOVE_NONE) {
        free(table.entries);
        return MOVE_NONE;
    }

    free(table.entries);
    return best_result.move;
}