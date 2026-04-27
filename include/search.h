#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"

#define MAX_PV_MOVES 128
#define MAX_ORDERED_MOVES 256

typedef struct {
    float score;
    Move move;
    Move pv[MAX_PV_MOVES];
    int pv_length;
    bool forced_root_move;
} SearchResult;

typedef struct {
    unsigned long long nodes;
    int seldepth;
} SearchStats;

typedef struct {
    bool hard_time_limited;
    long long hard_stop_time_ms;
    bool stop;
    volatile bool *external_stop;
} SearchControl;

typedef struct SearchContext SearchContext;

typedef void (*SearchMoveInfoCallback)(int depth,
                                       int move_number,
                                       Move move,
                                       float score,
                                       void *user_data);

SearchContext *search_context_create(int hash_mb); /* hash_mb: TT size in MB, 0 = default (64) */
void search_context_destroy(SearchContext *context);

SearchResult search_root(Board *board,
                         int depth,
                         RepetitionHistory *history,
                         SearchStats *stats,
                         SearchContext *context,
                         SearchControl *control,
                         SearchMoveInfoCallback on_move_info,
                         void *user_data);

/* Parallel root search helpers — used by think.c to distribute work across threads */
int search_root_generate_moves(Board *board, SearchContext *context,
                                Move ordered_moves[MAX_ORDERED_MOVES]);

SearchResult search_root_evaluate_move(const Board *board, Move move, int depth,
                                        float alpha, float beta,
                                        const RepetitionHistory *history,
                                        SearchStats *stats,
                                        SearchContext *context,
                                        SearchControl *control);

#endif
