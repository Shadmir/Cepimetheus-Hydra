#include "move.h"

#include <stddef.h>
#include <stdbool.h>

#include "board.h"

static void square_to_string(int square, char buffer[3]) {
    board_square_to_string(square, buffer);
}

void move_to_string(Move move, char buffer[6]) {
    char from[3];
    char to[3];
    square_to_string(move_from(move), from);
    square_to_string(move_to(move), to);
    buffer[0] = from[0];
    buffer[1] = from[1];
    buffer[2] = to[0];
    buffer[3] = to[1];
    switch (move_promotion(move)) {
        case MOVE_PROMO_KNIGHT: buffer[4] = 'n'; break;
        case MOVE_PROMO_BISHOP: buffer[4] = 'b'; break;
        case MOVE_PROMO_ROOK: buffer[4] = 'r'; break;
        case MOVE_PROMO_QUEEN: buffer[4] = 'q'; break;
        default: buffer[4] = '\0'; break;
    }
    if (buffer[4] == '\0') {
        buffer[5] = '\0';
    } else {
        buffer[5] = '\0';
    }
}

bool move_iscapture(const struct Board *board, Move move) {
    if (board == NULL) {
        return false;
    }
    if ((move_flags(move) & MOVE_FLAG_EN_PASSANT) != 0) {
        return true;
    }
    int from_piece = board_piece_at(board, move_from(move));
    if (from_piece < 0) {
        return false;
    }
    int target_piece = board_piece_at(board, move_to(move));
    return target_piece >= 0 && board_piece_color(target_piece) != board_piece_color(from_piece);
}

bool move_ischeck(const struct Board *board, Move move) {
    if (board == NULL) {
        return false;
    }
    Board temp = *board;
    Undo undo;
    if (!board_make_move(&temp, move, &undo)) {
        return false;
    }
    return board_is_in_check(&temp, temp.side);
}
