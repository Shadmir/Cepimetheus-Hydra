#ifndef EVAL_H
#define EVAL_H

#include "board.h"

bool eval_is_endgame_position(const Board *board);
float evaluate_position(Board *board, const RepetitionHistory *history, int ply);

#endif
