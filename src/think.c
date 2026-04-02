#include "think.h"

#include <time.h>

static unsigned int rng_state = 0;

static unsigned int rng_next(void) {
    if (rng_state == 0) {
        rng_state = (unsigned int)time(NULL) ^ 0x9e3779b9u;
    }
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

Move think(Board *board, const SearchLimits *limits) {
    (void)limits;

    MoveList list;
    movegen_generate_legal(board, &list);
    if (list.count <= 0) {
        return MOVE_NONE;
    }

    unsigned int index = rng_next() % (unsigned int)list.count;
    return list.moves[index];
}
