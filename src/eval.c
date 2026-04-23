#include "eval.h"

#include "movegen.h"
#include <stddef.h>

#define MATE_SCORE 100000.0f

/* File masks - one per file (A-H) */
static const U64 file_masks[8] = {
    0x0101010101010101ULL, /* A-file */
    0x0202020202020202ULL, /* B-file */
    0x0404040404040404ULL, /* C-file */
    0x0808080808080808ULL, /* D-file */
    0x1010101010101010ULL, /* E-file */
    0x2020202020202020ULL, /* F-file */
    0x4040404040404040ULL, /* G-file */
    0x8080808080808080ULL  /* H-file */
};

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

static int count_king_ring_attackers(const Board *board, int king_side) {
    int king_square = board->king_square[king_side];
    if (king_square < 0 || king_square >= 64) {
        return 0;
    }

    int attacker_side = (king_side == WHITE) ? BLACK : WHITE;
    U64 ring = bitboard_king_attacks(king_square);
    int attackers = 0;

    U64 bb = ring;
    while (bb) {
        int square = bitboard_pop_lsb(&bb);
        attackers += count_attackers_on_square(board, square, attacker_side);
    }

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
    if (rank >= 7) {
        return 0;
    }

    return 0xFFFFFFFFFFFFFFFFULL << ((rank + 1) * 8);
}

/* Get mask for all ranks ahead of given rank (for black pawns: 0 to rank-1) */
static U64 get_ranks_ahead_black(int rank) {
    if (rank <= 0) {
        return 0;
    }

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

        /* Create mask for current file and adjacent files. */
        U64 check_files = 0;
        if (file > 0) {
            check_files |= file_masks[file - 1];
        }
        check_files |= file_masks[file];
        if (file < 7) {
            check_files |= file_masks[file + 1];
        }

        /* Get mask for ranks ahead of this pawn. */
        U64 ranks_ahead = (side == WHITE) ? get_ranks_ahead_white(rank) : get_ranks_ahead_black(rank);

        /* Check if any enemy pawns exist in the check region. */
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
        if (endgame == 1) {
            /* Reward advanced pawns in the endgame. */
            piece_value += 2.0f * (float)pawn_rank;
            if (passed_pawns[square]) {
                /* Passed pawns are further rewarded for advancement. */
                piece_value += 4.0f * (float)pawn_rank;
            }
        }

        const int *pawns_per_file = is_white ? white_pawns_per_file : black_pawns_per_file;
        int this_file_count = pawns_per_file[file];

        if (this_file_count > 1) {
            /* Doubled pawn penalty: -5 points for each same-color pawn on the file. */
            piece_value -= 5.0f * (float)(this_file_count - 1);
        }

        bool has_left = (file > 0) && (pawns_per_file[file - 1] > 0);
        bool has_right = (file < 7) && (pawns_per_file[file + 1] > 0);
        if (!has_left && !has_right) {
            /* Isolated pawn penalty: -10 points if no same-color pawns on adjacent files. */
            piece_value -= 10.0f;
        }

        return piece_value;
    }

    if (type == WHITE_KNIGHT) {
        /* Knights on the rim are grim. */
        return piece_value + 3.0f * (float)popcount_u64(bitboard_knight_attacks(square));
    }

    if (type == WHITE_BISHOP) {
        /* Reward bishops with mobility through pawn occupancy only. 
        This is because a bishop on g2 with a knight on f3 is still good whereas if there was a pawn on f3 it would be blocked*/
        U64 pawn_occupancy = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
        return piece_value + 1.5f * (float)popcount_u64(bitboard_bishop_attacks(square, pawn_occupancy));
    }

    if (type == WHITE_ROOK) {
        if (endgame == -1) {
            /* Reward squares controlled. */
            piece_value += 1.0f * (float)popcount_u64(bitboard_rook_attacks(square, board->occupancy[BOTH]));

            U64 all_pawns = board->pieces[WHITE_PAWN] | board->pieces[BLACK_PAWN];
            U64 file_mask = file_masks[file];
            /* Open file bonus: +50 points if no pawns on the file. */
            if (popcount_u64(all_pawns & file_mask) == 0) {
                piece_value += 50.0f;
            }
        } else {
            /* Rooks are better in the endgame. */
            piece_value += 100.0f;
        }

        return piece_value;
    }

    if (type == WHITE_QUEEN) {
        return piece_value + 0.5f * (float)popcount_u64(bitboard_queen_attacks(square, board->occupancy[BOTH]));
    }

    /* If nothing prior, it is a king. */
    if (endgame == -1) {
        /* In opening/middlegame, king safety is important. */
        piece_value -= (float)popcount_u64(bitboard_queen_attacks(square, board->occupancy[BOTH]));
    }

    /* Calculate Manhattan distance to closest corner. */
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

    /* In endgames favor activity; in middlegames favor safety. */
    piece_value += (float)(endgame * corner_distance * 3);
    return piece_value;
}

bool eval_is_endgame_position(const Board *board) {
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

float evaluate_position(Board *board, const RepetitionHistory *history, int ply) {
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
            return -MATE_SCORE + (float)ply;
        }
    }

    int side_to_move = board->side;

    int endgame = eval_is_endgame_position(board) ? 1 : -1;

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

    /* Centre control. */
    static const int center_squares[4] = {27, 28, 35, 36}; /* d4, e4, d5, e5 */
    for (int i = 0; i < 4; ++i) {
        int square = center_squares[i];
        white_score += 3.0f * (float)count_attackers_on_square(board, square, WHITE);
        black_score += 3.0f * (float)count_attackers_on_square(board, square, BLACK);
    }

    white_score -= 2.0f * (float)count_king_ring_attackers(board, WHITE);
    black_score -= 2.0f * (float)count_king_ring_attackers(board, BLACK);

    /* Unless the position is zugzwang, having a move is often better. */
    float tempo_bonus = (endgame == -1) ? 10.0f : 0.0f;

    if (side_to_move == WHITE) {
        return white_score - black_score + tempo_bonus;
    }

    return black_score - white_score + tempo_bonus;
}
