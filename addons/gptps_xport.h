/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_xport.h - scale-OUT by composition: a worker-PROCESS transport (add-on).
 *
 * The gptps_pool add-on scales up by running N engines in ONE process. This one
 * scales OUT: it forks N persistent worker PROCESSES, and each submit is shipped to
 * a worker over IPC and its result marshalled back. Work therefore executes in a
 * SEPARATE address space (crash-isolated, independently capped) - and the local
 * socketpair here is the only thing standing between this and cross-MACHINE
 * execution: swap it for a TCP socket and the same protocol reaches another host.
 *
 * It is the reference case for the COMPOSITION pattern, and the distinction is worth
 * being exact about: this file consumes no seam at all. It never calls into an engine,
 * registers no task, constraint, observer or scheduler, and does not use the add-on
 * host-table ABI. The core is not on its path. That is the point rather than a gap -
 * the pattern needs nothing from the core, which is why the core offers it nothing.
 * (The four REAL seams are real because the core CALLS something: a task's run, a
 * constraint, a scheduler score, an observer. A transport sits on the other side of
 * the engine and calls IN, so an interface for it would be a vtable with no call site.)
 *
 * Like the pool, it needs NO core change - it is built on POSIX IPC plus a handler you
 * supply. The transport is the MECHANISM (route + marshal + supervise workers); how
 * a worker actually runs a task is your handler's business (dispatch on the name;
 * inside it you may drive a gptps engine, an executor, or plain C).
 *
 *   xp = gptps_xport_open(4, my_run, ud);       // 4 worker processes
 *   gptps_xport_submit(xp, "resize", buf, len, &res, &rlen, &task_status);
 *   free(res);
 *   gptps_xport_close(xp);                        // stops + reaps the workers
 *
 * POSIX only (fork + socketpair), like GPTPS_EXEC_OOP. The handler runs in a forked
 * child, so it must be fork-safe / self-contained (call gptps_xport_open before you
 * start other threads, or keep the handler independent of inherited threaded state).
 * The wire format is native-endian (a same-machine socketpair); a cross-host TCP
 * transport would serialise with a fixed byte order.
 */
#ifndef GPTPS_XPORT_H
#define GPTPS_XPORT_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_xport gptps_xport;

/* Cap on any single framed message (task name, payload, or result). A same-machine
 * worker runs your own forked handler, so this is defense-in-depth against a buggy
 * peer rather than a trust boundary - but it bounds a bad length field to a clean
 * failure instead of a multi-GB malloc, and is what a cross-host TCP variant of this
 * protocol would require anyway. It is public because submit() REJECTS an oversized
 * task name or payload (GPTPS_E_INVAL), so a caller has to be able to test for it. */
#define GPTPS_XPORT_MAX_MSG ((uint64_t)256u * 1024u * 1024u)

/* Runs one unit of work IN THE WORKER PROCESS. Return GPTPS_OK (or an error) and,
 * on success, a malloc'd result in *out_result (*out_len bytes) that the transport
 * sends back and then frees on the worker side. NULL/0 result is allowed. A result
 * larger than GPTPS_XPORT_MAX_MSG cannot be framed: the transport frees it and
 * replies GPTPS_E_BUDGET with no result, rather than dropping the link. */
typedef gptps_status (*gptps_xport_run_fn)(const char *task, const void *payload, size_t len,
                                           void **out_result, size_t *out_len, void *user_data);

/* Fork `nworkers` (>=1) worker processes, each looping on `run`. Returns NULL on a
 * bad argument or a fork/socket failure (any already-forked workers are reaped). */
gptps_xport *gptps_xport_open(size_t nworkers, gptps_xport_run_fn run, void *user_data);

/* Number of worker processes forked by open(). A worker whose link failed is retired
 * rather than respawned, and stays counted here - see submit(). */
size_t gptps_xport_count(gptps_xport *xp);

/* Ship one unit of work to a worker (round-robin) and BLOCK until it replies.
 * *out_result (may be NULL) receives a malloc'd result the CALLER frees; *out_len its
 * length. *out_task_status (may be NULL) receives the handler's own status. The return
 * value is the TRANSPORT status: GPTPS_OK if the round-trip completed (then check
 * out_task_status), GPTPS_E_IO if the worker died / the link failed, GPTPS_E_INVAL on
 * a bad argument - a NULL xp or task, a NULL payload with a nonzero len, or a task
 * name or payload above GPTPS_XPORT_MAX_MSG. Thread-safe: concurrent callers fan out
 * across the workers.
 *
 * A link that fails MID-FRAME cannot be resynchronised, so it is retired: that worker
 * is not respawned and every later submit routed to it returns GPTPS_E_IO. That is the
 * honest answer - the alternative is reading the abandoned frame's leftover bytes as
 * the next reply and returning a result for work that never ran. */
gptps_status gptps_xport_submit(gptps_xport *xp, const char *task,
                                const void *payload, size_t len,
                                void **out_result, size_t *out_len,
                                gptps_status *out_task_status);

/* Close each worker's link (the worker sees EOF and exits), reap them, and free. */
void gptps_xport_close(gptps_xport *xp);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_XPORT_H */
