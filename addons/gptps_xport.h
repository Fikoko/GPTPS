/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_xport.h - scale OUT by composition: N persistent worker PROCESSES, each with
 * its own address space, and - in engine mode - its own GPTPS engine.
 *
 * Where gptps_pool runs N engine shards inside one process, this forks N workers and
 * ships work to them over a socketpair. Two modes, chosen by what you put in the
 * config:
 *
 *   ENGINE MODE (gptps_xport_open_ex with `tasks`): every worker opens an engine,
 *   registers your task table, and runs whatever the link hands it THROUGH that
 *   engine. So the worker pool, the memory and named-resource budgets, retries,
 *   timeouts, dead-letter and the constraint/scheduler/observer seams all apply per
 *   worker process, exactly as they would in-process. Scaling out keeps everything
 *   GPTPS is for. The reply carries the item's terminal status (GPTPS_OK with the
 *   result on FINISHED; the failure status on DEAD_LETTERED / DROPPED; E_CANCELLED).
 *
 *   HANDLER MODE (gptps_xport_open, or open_ex with `run`): the worker runs a bare
 *   handler per request, one at a time, with no engine behind it. This is the
 *   original transport, kept unchanged for hosts that want plain crash-isolated RPC.
 *
 * The link is MULTIPLEXED: every request carries an id, a reader thread per link
 * matches replies to waiters, and up to `max_in_flight` requests may be outstanding
 * per worker (beyond that submit returns GPTPS_E_FULL - the same backpressure shape
 * as the core's limits.max_intake_depth). That is what makes engine mode useful: a
 * worker with 8 engine threads receives 8 concurrent requests, not one.
 *
 * Both blocking and asynchronous submission share that path:
 *   gptps_xport_submit()        blocks the caller until the reply
 *   gptps_xport_submit_async()  returns once the request is written; the reply
 *                               arrives on a callback
 *
 * What it still is not:
 *   - Not cross-host. Parent and child are the SAME forked binary, so the frames are
 *     native-endian and unversioned by construction - there is no second peer that
 *     could disagree. A cross-host transport needs gptps_remote's codec.
 *   - Not self-healing. A worker whose link breaks is retired for good; every
 *     request outstanding on it fails with GPTPS_E_IO. It is not respawned: the
 *     parent now has reader threads, and fork() from a multi-threaded process is
 *     only safe before those exist. gptps_xport_live() tells you how many remain.
 *   - POSIX only (fork + socketpair), like GPTPS_EXEC_OOP.
 *
 * Fork safety: gptps_xport_open* forks. Call it before your process starts other
 * threads (a forked child of a multi-threaded parent may deadlock on a lock some
 * other thread held). In engine mode the child opens a FRESH engine after the fork;
 * it never touches the parent's.
 *
 * Teardown: gptps_xport_close() is a graceful drain. It closes the request side of
 * every link; a worker in engine mode sees EOF, runs gptps_shutdown on its engine
 * (bounded by that engine's limits.shutdown_grace_ms - set engine_cfg->shutdown_grace_ms
 * if the default 30s is too long for you), sends the remaining replies, and exits.
 * Blocking submits outstanding at close time therefore get real answers; async
 * ones get their callback. Only then are the readers joined and the pids reaped.
 */
#ifndef GPTPS_XPORT_H
#define GPTPS_XPORT_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_xport gptps_xport;

/* Cap on any single framed message (task name, payload, or result). submit()
 * rejects an oversized argument locally with GPTPS_E_INVAL; a worker whose handler
 * produces a result over this answers GPTPS_E_BUDGET in frame. */
#define GPTPS_XPORT_MAX_MSG ((uint64_t)256u * 1024u * 1024u)

#define GPTPS_XPORT_DEFAULT_IN_FLIGHT 64u

/* HANDLER MODE: runs in the worker process, once per request, one at a time.
 * *out_result is malloc'd by the handler (or NULL); the transport frees it. */
typedef gptps_status (*gptps_xport_run_fn)(const char *task, const void *payload, size_t len,
                                           void **out_result, size_t *out_len, void *user_data);

/* ASYNC reply. Runs on the link's reader thread, with no transport lock held.
 *   io == GPTPS_OK   : the worker answered; task_status / res / len are the answer.
 *   io == GPTPS_E_IO : the link died first; task_status / res / len are unset.
 * `res` is valid only for the duration of the callback - copy it if you keep it.
 * The callback runs on a thread the transport needs, so it must not wait on the
 * transport. The blocking gptps_xport_submit returns GPTPS_E_BUSY here, because the
 * reply it would wait for can only be delivered by a reader thread - this one,
 * whenever the request lands on the callback's own link - except in an io ==
 * GPTPS_E_IO callback, whose link is already dead and so cannot be waited on (a
 * synchronous retry there is fine). gptps_xport_submit_async is allowed, but it
 * writes the request on this thread: keep it small, since a request larger than the
 * socket buffer can block while this link's worker is itself blocked sending a reply
 * that only this thread would read - hand large follow-ups to a thread of your own.
 * gptps_xport_close must not be called here at all: it joins this very thread. The
 * check covers this transport's own readers only; a callback that blocks on ANOTHER
 * transport whose callback blocks on this one can still deadlock. */
typedef void (*gptps_xport_reply_fn)(uint64_t request_id, gptps_status io,
                                     gptps_status task_status, const void *res, size_t len,
                                     void *user_data);

/* MEASUREMENTS (docs/MEASUREMENTS.md). In engine mode a worker's engine measures what
 * each process job used, and the reply carries the measurements of the item's LAST
 * attempt back: the FINISHED one, or the last FAILED one before its dead letter.
 * Unknown names pass through. In-process tasks, and handler mode, carry none. At most
 * GPTPS_XPORT_MAX_MEASURES travel with one reply, each name and method at most
 * GPTPS_XPORT_MEASURE_STR - 1 bytes; an item that reports more than that has the rest
 * left out, and `measures_cut` says so. */
#define GPTPS_XPORT_MAX_MEASURES 64u
#define GPTPS_XPORT_MEASURE_STR  256u

/* What the _ex calls report. In a callback every pointer is valid only for the call;
 * from gptps_xport_submit_ex the result and the measurements are the caller's, freed
 * with gptps_xport_result_free. */
typedef struct {
    size_t               struct_size;   /* = sizeof(gptps_xport_reply) */
    uint64_t             request_id;
    gptps_status         io;            /* GPTPS_OK: the worker answered; GPTPS_E_IO: the
                                         * link died first, and the rest is unset */
    gptps_status         task_status;   /* the item's own status, as for gptps_xport_submit */
    const void          *result;
    size_t               result_len;
    const gptps_measure *measures;      /* NULL / 0 when nothing was measured */
    size_t               n_measures;
    int                  measures_cut;  /* the worker left some out (see above) */
} gptps_xport_reply;

typedef void (*gptps_xport_reply_ex_fn)(const gptps_xport_reply *reply, void *user_data);

/* ENGINE MODE hook: runs in EACH worker process, after its engine is open and the
 * task table registered, before the first request. Define named resources, register
 * constraints, install gptps_stats / gptps_durable_queue, set priorities - anything
 * you would do to an in-process engine between open and the first submit. */
typedef void (*gptps_xport_child_init_fn)(gptps *worker_engine, void *user_data);

typedef struct {
    size_t   struct_size;              /* = sizeof(gptps_xport_config) */
    size_t   nworkers;                 /* >= 1 */
    uint32_t max_in_flight;            /* per worker link; 0 => GPTPS_XPORT_DEFAULT_IN_FLIGHT.
                                        * Beyond it, submit returns GPTPS_E_FULL. */

    /* HANDLER MODE - set `run`, leave `tasks` NULL. */
    gptps_xport_run_fn  run;
    void               *user_data;

    /* ENGINE MODE - set `tasks`/`ntasks`, leave `run` NULL. Setting both is
     * GPTPS_E_INVAL (open returns NULL). */
    const gptps_config     *engine_cfg;   /* NULL => each worker auto-tunes to the WHOLE
                                           * machine - which oversubscribes for N > 1,
                                           * exactly like gptps_pool. Size the limits so
                                           * the workers SUM to what the box can bear. A
                                           * config_path here is read by every worker,
                                           * add-ons included, as gptps_open_ex reads it. */
    const gptps_task_def   *tasks;        /* registered, in order, on every worker's engine */
    size_t                  ntasks;
    gptps_xport_child_init_fn child_init; /* optional */
    void                   *child_init_ud;
} gptps_xport_config;

/* Open N workers. NULL on any failure (nothing is left running). */
gptps_xport *gptps_xport_open_ex(const gptps_xport_config *cfg);

/* HANDLER MODE shorthand, unchanged from before: N workers running `run`. */
gptps_xport *gptps_xport_open(size_t nworkers, gptps_xport_run_fn run, void *user_data);

/* Workers this transport was created with - the pool SIZE, which never changes. */
size_t gptps_xport_count(gptps_xport *xp);

/* Workers still able to take work. Only ever falls; at 0 every submit is E_IO. */
size_t gptps_xport_live(gptps_xport *xp);

/* Requests outstanding right now across all live links. */
size_t gptps_xport_in_flight(gptps_xport *xp);

/* Round-robin a request to the next live worker and BLOCK until its reply.
 * Returns GPTPS_OK when the worker answered (then *out_task_status is the task's
 * own status, *out_result the malloc'd result or NULL - free it), GPTPS_E_IO when
 * the link broke first, GPTPS_E_FULL when that worker already has max_in_flight
 * outstanding, GPTPS_E_INVAL for a bad argument, GPTPS_E_BUSY from a reply callback
 * (see gptps_xport_reply_fn). */
gptps_status gptps_xport_submit(gptps_xport *xp, const char *task,
                                const void *payload, size_t len,
                                void **out_result, size_t *out_len,
                                gptps_status *out_task_status);

/* Same routing and backpressure, but returns once the request is on the wire (or its
 * link has already failed it - see below); `cb` gets the reply (or E_IO).
 * *out_request_id (may be NULL) identifies it.
 * Every call has exactly ONE outcome: a non-OK return and no callback, or GPTPS_OK
 * and exactly one callback. A link that dies while this request is being written
 * can surface either way - as GPTPS_E_IO here, or as GPTPS_OK with the callback
 * carrying io == GPTPS_E_IO, possibly before this call returns - never both. A
 * GPTPS_E_IO return means the request never reached a worker; an E_IO callback
 * does not say whether it ran. */
gptps_status gptps_xport_submit_async(gptps_xport *xp, const char *task,
                                      const void *payload, size_t len,
                                      gptps_xport_reply_fn cb, void *user_data,
                                      uint64_t *out_request_id);

/* gptps_xport_submit, with the reply's measurements. *out is filled (its struct_size
 * set by the caller) when this returns GPTPS_OK; free it with gptps_xport_result_free.
 * The same returns as gptps_xport_submit otherwise, and *out is then left empty. */
gptps_status gptps_xport_submit_ex(gptps_xport *xp, const char *task,
                                   const void *payload, size_t len, gptps_xport_reply *out);
/* Free what gptps_xport_submit_ex put in *r (the result and the measurements), and
 * empty it. Safe on an empty or already freed reply. */
void gptps_xport_result_free(gptps_xport_reply *r);

/* gptps_xport_submit_async, with the reply's measurements: the same one-outcome rule,
 * and the same rules for what the callback may do. */
gptps_status gptps_xport_submit_async_ex(gptps_xport *xp, const char *task,
                                         const void *payload, size_t len,
                                         gptps_xport_reply_ex_fn cb, void *user_data,
                                         uint64_t *out_request_id);

/* Graceful drain (see header), then reap and free. Not from a reply callback. */
void gptps_xport_close(gptps_xport *xp);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_XPORT_H */
