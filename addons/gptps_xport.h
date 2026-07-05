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
 * It is the reference consumer of the transport seam (GPTPS_SEAM_TRANSPORT) and,
 * like the pool, needs NO core change - it is built on POSIX IPC plus a handler you
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

/* Runs one unit of work IN THE WORKER PROCESS. Return GPTPS_OK (or an error) and,
 * on success, a malloc'd result in *out_result (*out_len bytes) that the transport
 * sends back and then frees on the worker side. NULL/0 result is allowed. */
typedef gptps_status (*gptps_xport_run_fn)(const char *task, const void *payload, size_t len,
                                           void **out_result, size_t *out_len, void *user_data);

/* Fork `nworkers` (>=1) worker processes, each looping on `run`. Returns NULL on a
 * bad argument or a fork/socket failure (any already-forked workers are reaped). */
gptps_xport *gptps_xport_open(size_t nworkers, gptps_xport_run_fn run, void *user_data);

/* Number of live workers. */
size_t gptps_xport_count(gptps_xport *xp);

/* Ship one unit of work to a worker (round-robin) and BLOCK until it replies.
 * *out_result (may be NULL) receives a malloc'd result the CALLER frees; *out_len its
 * length. *out_task_status (may be NULL) receives the handler's own status. The return
 * value is the TRANSPORT status: GPTPS_OK if the round-trip completed (then check
 * out_task_status), GPTPS_E_IO if the worker died / the link failed, GPTPS_E_INVAL on
 * a bad argument. Thread-safe: concurrent callers fan out across the workers. */
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
