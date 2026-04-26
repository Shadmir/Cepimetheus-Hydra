#ifndef THINK_H
#define THINK_H

#include "movegen.h"

typedef struct SearchOptions {
    int overhead_ms;
    int threads;
    int hash_mb;
} SearchOptions;

typedef struct SearchLimits {
    int depth;
    int movetime_ms;
    int wtime_ms;
    int btime_ms;
    int winc_ms;
    int binc_ms;
    int movestogo;
    bool has_clock_time;
    bool infinite;
} SearchLimits;

/* Persistent SMP worker thread pool — create once, reuse across moves. */
typedef struct SmpThreadPool SmpThreadPool;

SmpThreadPool *smp_thread_pool_create(int num_workers);
void           smp_thread_pool_destroy(SmpThreadPool *pool);

Move think(Board *board,
           const SearchLimits *limits,
           const SearchOptions *options,
           const RepetitionHistory *history,
           volatile bool *stop_signal,
           SmpThreadPool *pool);

#endif
