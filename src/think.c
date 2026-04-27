#include "think.h"
#include "../include/search.h"

#include <float.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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


/* ---------- Parallel root move search ---------- */

/*
 * Shared state for one depth iteration. The main thread populates the move
 * queue and resets this before signalling workers. All threads (main + workers)
 * drain the queue concurrently: each grabs one move, searches it, reports back.
 * The mutex protects the queue index and the best-result fields.
 */
typedef struct {
    pthread_mutex_t mutex;

    /* Move queue — populated once per depth by main thread */
    Move ordered_moves[MAX_ORDERED_MOVES];
    int  num_moves;
    int  next_move_idx; /* next unassigned slot */

    /* Rolling best result — updated under mutex as threads finish moves */
    float best_score;
    Move  best_move;
    Move  best_pv[MAX_PV_MOVES];
    int   best_pv_length;

    /* Read-only inputs — workers make local copies before modifying */
    Board             board;
    RepetitionHistory history;
    SearchContext    *context;
    int               depth;
    volatile bool    *stop; /* points to think()'s smp_stop */
} ParallelRootState;

static void drain_root_move_queue(ParallelRootState *state,
                                  SearchControl *control,
                                  SearchStats *stats) {
    for (;;) {
        pthread_mutex_lock(&state->mutex);
        bool ext_stop  = (state->stop != NULL && *state->stop);
        bool ctrl_stop = (control != NULL && control->stop);
        if (ext_stop || ctrl_stop || state->next_move_idx >= state->num_moves) {
            /* Propagate hard-time-limit stop to all other threads */
            if (ctrl_stop && state->stop != NULL) {
                *state->stop = true;
            }
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        int   idx      = state->next_move_idx++;
        float my_alpha = state->best_score; /* lazy: use current best as alpha */
        pthread_mutex_unlock(&state->mutex);

        Move move = state->ordered_moves[idx];
        SearchResult r = search_root_evaluate_move(
            &state->board, move, state->depth,
            my_alpha, FLT_MAX,
            &state->history, stats, state->context, control);

        if (r.move == MOVE_NONE) {
            continue;
        }

        pthread_mutex_lock(&state->mutex);
        if (r.score > state->best_score || state->best_move == MOVE_NONE) {
            state->best_score     = r.score;
            state->best_move      = move;
            state->best_pv[0]     = move;
            state->best_pv_length = 1;
            for (int i = 0; i < r.pv_length && state->best_pv_length < MAX_PV_MOVES; ++i) {
                state->best_pv[state->best_pv_length++] = r.pv[i];
            }
        }
        pthread_mutex_unlock(&state->mutex);
    }
}

/* ---------- Persistent SMP thread pool ---------- */

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  start_cond; /* main → worker: depth ready */
    pthread_cond_t  done_cond;  /* worker → main: depth done  */

    ParallelRootState *root_state; /* set by main before each depth signal */
    SearchControl      control;
    SearchStats        stats;      /* per-worker stats for the current depth */

    bool should_search;
    bool is_searching;
    bool quit;
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
        bool do_quit    = w->quit;
        w->is_searching = !do_quit;
        pthread_mutex_unlock(&w->mutex);

        if (do_quit) break;

        w->stats = (SearchStats){0};
        drain_root_move_queue(w->root_state, &w->control, &w->stats);

        pthread_mutex_lock(&w->mutex);
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

    int hash_mb = (options != NULL && options->hash_mb > 0) ? options->hash_mb : 0;
    SearchContext *search_context = search_context_create(hash_mb);

    int num_workers = (pool != NULL) ? pool->num_workers : 0;

    SearchResult best_result = {0.0f, MOVE_NONE, {0}, 0, false};

    int depth_limit = target_depth;
    if (infinite_search && !depth_explicitly_set && !time_limited && movetime_ms <= 0) {
        depth_limit = INT_MAX;
    }

    if (num_workers == 0) {
        /* ---- Single-threaded path (unchanged behaviour) ---- */
        for (int depth = 1; depth <= depth_limit; ++depth) {
            if (stop_signal != NULL && *stop_signal) {
                break;
            }

            if (time_limited) {
                long long elapsed_before = current_time_ms() - start_time_ms;
                if (elapsed_before < 0) elapsed_before = 0;
                if (elapsed_before >= soft_time_limit_ms && best_result.move != MOVE_NONE) {
                    break;
                }
            }

            SearchStats stats = {0ULL, depth};
            control.stop = false;

            SearchResult result = search_root(board, depth, &search_history,
                                              &stats, search_context, &control,
                                              print_move_info_callback, NULL);

            if (control.stop) break;

            long long elapsed_ms = current_time_ms() - start_time_ms;
            if (elapsed_ms < 0) elapsed_ms = 0;

            print_depth_info(depth, &result, &stats, elapsed_ms);

            if (result.move != MOVE_NONE) {
                best_result = result;
            }

            if (result.forced_root_move) break;

            if (time_limited && elapsed_ms >= soft_time_limit_ms) break;
        }
    } else {
        /* ---- Parallel root-move search ---- */
        volatile bool smp_stop = false;

        ParallelRootState root_state;
        memset(&root_state, 0, sizeof(root_state));
        pthread_mutex_init(&root_state.mutex, NULL);
        root_state.board   = *board;
        root_state.history = search_history;
        root_state.context = search_context;
        root_state.stop    = &smp_stop;

        /* Worker control: external_stop wired to smp_stop so workers abort
         * immediately when the main thread signals stop. */
        SearchControl worker_control = control;
        worker_control.external_stop = &smp_stop;

        for (int depth = 1; depth <= depth_limit; ++depth) {
            if (stop_signal != NULL && *stop_signal) {
                smp_stop = true;
                break;
            }

            if (time_limited) {
                long long elapsed_before = current_time_ms() - start_time_ms;
                if (elapsed_before < 0) elapsed_before = 0;
                if (elapsed_before >= soft_time_limit_ms && best_result.move != MOVE_NONE) {
                    smp_stop = true;
                    break;
                }
            }

            /* Populate the move queue for this depth */
            pthread_mutex_lock(&root_state.mutex);
            root_state.depth          = depth;
            root_state.next_move_idx  = 0;
            root_state.best_score     = -FLT_MAX;
            root_state.best_move      = MOVE_NONE;
            root_state.best_pv_length = 0;
            root_state.num_moves = search_root_generate_moves(
                board, search_context, root_state.ordered_moves);
            pthread_mutex_unlock(&root_state.mutex);

            /* Forced/no-move cases */
            if (root_state.num_moves == 0) break;
            if (root_state.num_moves == 1) {
                if (best_result.move == MOVE_NONE) {
                    best_result.move = root_state.ordered_moves[0];
                }
                best_result.forced_root_move = true;
                smp_stop = true;
                break;
            }

            /* Reset worker control stop flag */
            worker_control.stop = false;
            control.stop = false;

            /* Signal workers to start this depth */
            for (int i = 0; i < num_workers; i++) {
                SmpWorker *w = &pool->workers[i];
                pthread_mutex_lock(&w->mutex);
                w->root_state    = &root_state;
                w->control       = worker_control;
                w->should_search = true;
                pthread_cond_signal(&w->start_cond);
                pthread_mutex_unlock(&w->mutex);
            }

            /* Main thread also drains the queue */
            SearchStats main_stats = {0ULL, depth};
            drain_root_move_queue(&root_state, &control, &main_stats);

            /* Wait for all workers to finish this depth */
            for (int i = 0; i < num_workers; i++) {
                SmpWorker *w = &pool->workers[i];
                pthread_mutex_lock(&w->mutex);
                while (w->is_searching) {
                    pthread_cond_wait(&w->done_cond, &w->mutex);
                }
                pthread_mutex_unlock(&w->mutex);
            }

            /* If stopped mid-depth, use best result from previous depth */
            if (smp_stop || (stop_signal != NULL && *stop_signal)) {
                break;
            }

            /* Aggregate stats from all threads */
            SearchStats total_stats = main_stats;
            for (int i = 0; i < num_workers; i++) {
                total_stats.nodes += pool->workers[i].stats.nodes;
                if (pool->workers[i].stats.seldepth > total_stats.seldepth) {
                    total_stats.seldepth = pool->workers[i].stats.seldepth;
                }
            }

            long long elapsed_ms = current_time_ms() - start_time_ms;
            if (elapsed_ms < 0) elapsed_ms = 0;

            if (root_state.best_move != MOVE_NONE) {
                best_result.score     = root_state.best_score;
                best_result.move      = root_state.best_move;
                best_result.pv_length = root_state.best_pv_length;
                memcpy(best_result.pv, root_state.best_pv,
                       (size_t)root_state.best_pv_length * sizeof(Move));
                best_result.forced_root_move = false;
            }

            print_depth_info(depth, &best_result, &total_stats, elapsed_ms);

            if (time_limited && elapsed_ms >= soft_time_limit_ms) {
                smp_stop = true;
                break;
            }
        }

        /* Ensure all workers are idle before we free the shared context */
        smp_stop = true;
        for (int i = 0; i < num_workers; i++) {
            SmpWorker *w = &pool->workers[i];
            pthread_mutex_lock(&w->mutex);
            while (w->is_searching) {
                pthread_cond_wait(&w->done_cond, &w->mutex);
            }
            pthread_mutex_unlock(&w->mutex);
        }

        pthread_mutex_destroy(&root_state.mutex);
    }

    search_context_destroy(search_context);
    return best_result.move;
}