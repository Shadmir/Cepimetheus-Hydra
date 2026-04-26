#include "think.h"
#include "../include/search.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

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

static void print_move_info_callback(int depth,
                                     int move_number,
                                     Move move,
                                     float score,
                                     void *user_data) {
    (void)user_data;
    print_move_info(depth, move_number, move, score);
}

static long long current_time_ms(void) {
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return 0;
    }

    return (long long)now.tv_sec * 1000LL + (long long)now.tv_usec / 1000LL;
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

static int clamp_time_budget(int budget_ms) {
    if (budget_ms < 10) {
        return 10;
    }

    return budget_ms;
}

static bool compute_clock_budget(const Board *board,
                                 const SearchLimits *limits,
                                 const SearchOptions *options,
                                 int *soft_budget_ms,
                                 int *hard_budget_ms) {
    if (board == NULL || limits == NULL || soft_budget_ms == NULL || hard_budget_ms == NULL) {
        return false;
    }

    int overhead_ms = 50;
    if (options != NULL) {
        overhead_ms = options->overhead_ms;
    }

    int base_ms = 0;
    int increment_ms = 0;

    if (board->side == WHITE) {
        base_ms = limits->wtime_ms;
        increment_ms = limits->winc_ms;
    } else {
        base_ms = limits->btime_ms;
        increment_ms = limits->binc_ms;
    }

    int total_ms = base_ms + increment_ms;
    int soft_ms = (total_ms / 30) - overhead_ms;
    int hard_ms = (total_ms / 10) - overhead_ms;

    if (hard_ms < soft_ms) {
        hard_ms = soft_ms;
    }

    *soft_budget_ms = clamp_time_budget(soft_ms);
    *hard_budget_ms = clamp_time_budget(hard_ms);
    return true;
}


/* ---------- Persistent SMP thread pool ---------- */

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  start_cond;   /* main  → worker: new search ready */
    pthread_cond_t  done_cond;    /* worker → main:  search finished  */

    /* Task data written by main before signalling start_cond */
    Board              board;
    RepetitionHistory  history;
    SearchContext     *context;   /* shared TT, not owned */
    SearchControl      control;
    int                start_depth;
    volatile bool     *smp_stop;  /* points into think()'s stack frame */

    /* State flags (protected by mutex) */
    bool should_search;
    bool is_searching;
    bool quit;

    /* Output written by worker after each search */
    unsigned long long total_nodes;
} SmpWorker;

struct SmpThreadPool {
    SmpWorker *workers;
    int        num_workers;
};

static void *smp_worker_main(void *arg) {
    SmpWorker *w = (SmpWorker *)arg;

    for (;;) {
        pthread_mutex_lock(&w->mutex);
        while (!w->should_search && !w->quit) {
            pthread_cond_wait(&w->start_cond, &w->mutex);
        }
        bool do_quit = w->quit;
        w->is_searching = !do_quit;
        pthread_mutex_unlock(&w->mutex);

        if (do_quit) break;

        unsigned long long total_nodes = 0;
        SearchStats stats = {0};
        int start = w->start_depth > 0 ? w->start_depth : 1;
        for (int depth = start; depth < INT_MAX; depth++) {
            bool should_stop = w->control.stop;
            if (!should_stop && w->smp_stop != NULL) {
                should_stop = *w->smp_stop;
            }
            if (should_stop) break;
            stats.nodes = 0;
            search_root(&w->board, depth, &w->history, &stats,
                        w->context, &w->control, NULL, NULL);
            total_nodes += stats.nodes;
            if (w->control.stop) break;
        }

        pthread_mutex_lock(&w->mutex);
        w->total_nodes   = total_nodes;
        w->should_search = false;
        w->is_searching  = false;
        pthread_cond_signal(&w->done_cond);
        pthread_mutex_unlock(&w->mutex);
    }
    return NULL;
}

SmpThreadPool *smp_thread_pool_create(int num_workers) {
    if (num_workers <= 0) {
        SmpThreadPool *pool = calloc(1, sizeof(*pool));
        return pool;
    }

    SmpThreadPool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) return NULL;

    pool->workers = calloc((size_t)num_workers, sizeof(SmpWorker));
    if (pool->workers == NULL) {
        free(pool);
        return NULL;
    }
    pool->num_workers = num_workers;

    for (int i = 0; i < num_workers; i++) {
        SmpWorker *w = &pool->workers[i];
        pthread_mutex_init(&w->mutex, NULL);
        pthread_cond_init(&w->start_cond, NULL);
        pthread_cond_init(&w->done_cond, NULL);
        if (pthread_create(&w->thread, NULL, smp_worker_main, w) != 0) {
            /* Shrink pool to successfully-started workers */
            pool->num_workers = i;
            break;
        }
    }

    return pool;
}

void smp_thread_pool_destroy(SmpThreadPool *pool) {
    if (pool == NULL) return;

    for (int i = 0; i < pool->num_workers; i++) {
        SmpWorker *w = &pool->workers[i];
        pthread_mutex_lock(&w->mutex);
        w->quit = true;
        pthread_cond_signal(&w->start_cond);
        pthread_mutex_unlock(&w->mutex);
        pthread_join(w->thread, NULL);
        pthread_mutex_destroy(&w->mutex);
        pthread_cond_destroy(&w->start_cond);
        pthread_cond_destroy(&w->done_cond);
    }

    free(pool->workers);
    free(pool);
}

Move think(Board *board,
           const SearchLimits *limits,
           const SearchOptions *options,
           const RepetitionHistory *history,
           volatile bool *stop_signal,
           SmpThreadPool *pool) {
    if (board == NULL) {
        return MOVE_NONE;
    }

    int target_depth = 4; //Default if no limits provided
    int movetime_ms = 0;
    int soft_time_limit_ms = 0;
    int hard_time_limit_ms = 0;
    int overhead_ms = 50;
    bool depth_explicitly_set = false;
    bool time_limited = false;
    bool infinite_search = false;
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
        } else if (limits->has_clock_time && compute_clock_budget(board, limits, options, &soft_time_limit_ms, &hard_time_limit_ms)) {
            time_limited = true;
        }

        if (limits->infinite) {
            infinite_search = true;
        }
    }

    if (movetime_ms > 0 && !depth_explicitly_set) {
        /* In pure movetime mode, deepen until the time budget is consumed. */
        target_depth = max_iterative_depth;
    }

    if (movetime_ms > 0) {
        soft_time_limit_ms = movetime_ms - overhead_ms;
        hard_time_limit_ms = soft_time_limit_ms;
        soft_time_limit_ms = clamp_time_budget(soft_time_limit_ms);
        hard_time_limit_ms = clamp_time_budget(hard_time_limit_ms);
        time_limited = true;
    } else if (time_limited && !depth_explicitly_set) {
        target_depth = max_iterative_depth;
    }

    RepetitionHistory search_history;
    repetition_history_init(&search_history);
    if (history != NULL) {
        search_history = *history;
    }

    long long start_time_ms = current_time_ms();

    SearchControl control = {0};
    control.external_stop = stop_signal;
    if (time_limited) {
        control.hard_time_limited = true;
        control.hard_stop_time_ms = start_time_ms + (long long)hard_time_limit_ms;
    }

    SearchContext *search_context = search_context_create();

    /* Lazy SMP: signal persistent worker threads to start searching */
    volatile bool smp_stop = false;
    int num_workers = (pool != NULL) ? pool->num_workers : 0;

    for (int i = 0; i < num_workers; i++) {
        SmpWorker *w = &pool->workers[i];
        pthread_mutex_lock(&w->mutex);
        w->board       = *board;
        w->history     = search_history;
        w->context     = search_context;
        w->control     = (SearchControl){0};
        w->smp_stop    = &smp_stop;
        w->start_depth = 1 + (i % 3); /* stagger: depths 1, 2, 3, 1, 2, 3... */
        w->total_nodes = 0;
        w->should_search = true;
        pthread_cond_signal(&w->start_cond);
        pthread_mutex_unlock(&w->mutex);
    }

    SearchResult best_result = {0.0f, MOVE_NONE, {0}, 0, false};
    SearchResult result = {0.0f, MOVE_NONE, {0}, 0, false};

    /* Iterative deepening: search depths 1 through target_depth. */
    int depth_limit = target_depth;
    if (infinite_search && !depth_explicitly_set && !time_limited && movetime_ms <= 0) {
        depth_limit = INT_MAX;
    }

    for (int depth = 1; depth <= depth_limit; ++depth) {
        if (stop_signal != NULL && *stop_signal) {
            smp_stop = true;
            break;
        }

        if (time_limited) {
            long long elapsed_before_depth_ms = current_time_ms() - start_time_ms;
            if (elapsed_before_depth_ms < 0) {
                elapsed_before_depth_ms = 0;
            }

            if (elapsed_before_depth_ms >= soft_time_limit_ms && best_result.move != MOVE_NONE) {
                smp_stop = true;
                break;
            }
        }

        SearchStats stats = {0ULL, depth};
        control.stop = false;

        result = search_root(board,
                             depth,
                             &search_history,
                             &stats,
                             search_context,
                             &control,
                             print_move_info_callback,
                             NULL);

        if (control.stop) {
            smp_stop = true;
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

        // If the root search resulted in only one legal move then we can stop searching immediately, no matter the time limits, as we won't find a better move by searching deeper
        if (result.forced_root_move) {
            smp_stop = true;
            break;
        }

        /* Check if we should stop searching */
        if (time_limited && elapsed_ms >= soft_time_limit_ms) {
            smp_stop = true;
            break;
        }
    }

    /* Signal workers to stop and wait for them to finish before destroying shared context */
    smp_stop = true;
    unsigned long long worker_nodes = 0;
    for (int i = 0; i < num_workers; i++) {
        SmpWorker *w = &pool->workers[i];
        pthread_mutex_lock(&w->mutex);
        while (w->is_searching) {
            pthread_cond_wait(&w->done_cond, &w->mutex);
        }
        worker_nodes += w->total_nodes;
        pthread_mutex_unlock(&w->mutex);
    }

    /* Print worker thread totals so multi-thread search depth is visible */
    if (worker_nodes > 0) {
        long long elapsed_ms = current_time_ms() - start_time_ms;
        if (elapsed_ms < 0) elapsed_ms = 0;
        unsigned long long total_nps = elapsed_ms > 0
            ? (worker_nodes * 1000ULL) / (unsigned long long)elapsed_ms : 0;
        printf("info string worker_nodes %llu nps %llu\n", worker_nodes, total_nps);
        fflush(stdout);
    }

    if (best_result.move == MOVE_NONE) {
        search_context_destroy(search_context);
        return MOVE_NONE;
    }

    search_context_destroy(search_context);
    return best_result.move;
}