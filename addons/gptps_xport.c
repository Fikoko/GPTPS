/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_xport.c - worker-process transport (scale-out) built on POSIX IPC.
 *
 * open() creates N socketpairs and forks a worker for each - all forks happen BEFORE
 * any thread is started here, so every child is born from a parent whose only
 * xport threads are none. Then one READER thread per link is started in the parent.
 *
 * Wire (native-endian, unversioned: both ends are the same forked binary):
 *   request  [u64 id][u32 tlen][task][u64 plen][payload]
 *   reply    [u64 id][i32 status][u64 rlen][result]
 *
 * PARENT: submit() registers a pending record under the link's pmu (bounded by
 * max_in_flight -> E_FULL), writes the frame under the link's wmu, and either waits
 * on the link's condvar (blocking) or returns (async). The reader thread reads
 * replies, matches them by id, and completes the record: fills + broadcasts for a
 * blocking waiter, or invokes the callback for an async one. On EOF/error it marks
 * the link dead and fails every outstanding record with E_IO.
 *
 * Locks: wmu (frame writes) and pmu (pending list + liveness) are never held at the
 * same time by anyone, and cursor_lock (rotation) is never held while acquiring
 * either. The reader takes pmu only. A callback runs with no lock held.
 *
 * WORKER, engine mode: the main thread reads requests and gptps_submit()s them to
 * the worker's own engine, recording handle -> request id. The engine's event
 * callback (dispatcher thread) writes the reply on the item's terminal event. EOF on
 * the link => gptps_shutdown (bounded drain; remaining replies are sent) => _exit.
 *
 * WORKER, handler mode: the main thread reads a request, runs the handler, writes
 * the reply. Sequential, as the original transport was.
 *
 * No core change; everything here is POSIX + the public header.
 */
#if defined(_WIN32)
/* fork()/socketpair() have no Win32 equivalent; this reference add-on is POSIX-only
 * (like GPTPS_EXEC_OOP). It compiles to nothing on Windows. */
#else

#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif

#include "gptps_xport.h"
#include "addon_compat.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/wait.h>

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0   /* macOS/BSD: rely on SO_NOSIGPIPE (set below) instead */
#endif

/* ============================================================================
 * framed socket I/O (EINTR-safe; SIGPIPE suppressed per-call)
 * ==========================================================================*/
static int sock_write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf; size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}
static int sock_read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf; size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;   /* peer closed */
        off += (size_t)r;
    }
    return 0;
}
static void set_nosigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)   /* macOS/BSD per-socket SIGPIPE suppression */
    int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

/* One reply frame. The status is clamped to the frame cap on the way out so the
 * channel stays byte-synchronised whatever the result size (see the worker). */
static int write_reply(int fd, uint64_t id, int32_t st, const void *res, uint64_t rlen)
{
    if (sock_write_all(fd, &id, sizeof id) != 0) return -1;
    if (sock_write_all(fd, &st, sizeof st) != 0) return -1;
    if (sock_write_all(fd, &rlen, sizeof rlen) != 0) return -1;
    if (rlen && sock_write_all(fd, res, (size_t)rlen) != 0) return -1;
    return 0;
}

/* Read one request. Returns 0 with malloc'd task and payload, -1 on EOF/error/bad
 * length (the caller drops the link: a stream cannot resynchronise mid-frame). */
static int read_request(int fd, uint64_t *id, char **task, void **payload, uint64_t *plen)
{
    uint32_t tlen = 0;
    *task = NULL; *payload = NULL; *plen = 0;
    if (sock_read_all(fd, id, sizeof *id) != 0) return -1;
    if (sock_read_all(fd, &tlen, sizeof tlen) != 0) return -1;
    if (tlen > GPTPS_XPORT_MAX_MSG) return -1;
    *task = (char *)malloc((size_t)tlen + 1);
    if (!*task || sock_read_all(fd, *task, tlen) != 0) { free(*task); *task = NULL; return -1; }
    (*task)[tlen] = 0;
    if (sock_read_all(fd, plen, sizeof *plen) != 0 || *plen > GPTPS_XPORT_MAX_MSG) { free(*task); *task = NULL; return -1; }
    if (*plen) {
        *payload = malloc((size_t)*plen);
        if (!*payload || sock_read_all(fd, *payload, (size_t)*plen) != 0) {
            free(*task); free(*payload); *task = NULL; *payload = NULL; return -1;
        }
    }
    return 0;
}

/* ============================================================================
 * WORKER, handler mode (runs in the forked child; never returns)
 * ==========================================================================*/
static void worker_handler_main(int fd, gptps_xport_run_fn run, void *ud)
{
    for (;;) {
        uint64_t id, plen, rl;
        char *task; void *payload, *res = NULL; size_t rlen = 0;
        int32_t st32;

        if (read_request(fd, &id, &task, &payload, &plen) != 0) break;   /* EOF => shut down */
        st32 = (int32_t)run(task, payload, (size_t)plen, &res, &rlen, ud);
        rl = (uint64_t)rlen;
        if (rl > GPTPS_XPORT_MAX_MSG) {
            /* Over the frame cap: shipping it would make the parent abandon the frame
             * mid-way and poison every later reply on this link. Answer IN FRAME with
             * an empty body instead: an honest status, and the channel stays in sync. */
            free(res); res = NULL; st32 = (int32_t)GPTPS_E_BUDGET; rl = 0;
        }
        {   int w = write_reply(fd, id, st32, res, rl);
            free(res); free(payload); free(task);
            if (w != 0) break; }
    }
    close(fd);
    _exit(0);
}

/* ============================================================================
 * WORKER, engine mode (runs in the forked child; never returns)
 * ==========================================================================*/
typedef struct { gptps_handle h; uint64_t id; } inflight;

typedef struct {
    int             fd;
    apx_mutex       wmu;        /* frame writes: the event callback vs. immediate rejections */
    apx_mutex       mmu;        /* the handle -> id table */
    inflight       *tab;
    size_t          n, cap;
} worker_engine_ctx;

static int is_terminal(const gptps_event *ev)
{
    if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DROPPED ||
        ev->kind == GPTPS_EV_DEAD_LETTERED) return 1;
    return ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED;
}

/* Dispatcher thread. The table lookup can block briefly on mmu while the main
 * thread is inside gptps_submit + insert; that is deliberate (see the main loop) and
 * safe because the core emits events with its own lock released. */
static void worker_engine_ev(const gptps_event *ev, void *ud)
{
    worker_engine_ctx *c = (worker_engine_ctx *)ud;
    size_t i; uint64_t id = 0; int found = 0;
    int32_t st;
    if (!is_terminal(ev)) return;

    apx_mutex_lock(&c->mmu);
    for (i = 0; i < c->n; ++i)
        if (c->tab[i].h == ev->handle) { id = c->tab[i].id; c->tab[i] = c->tab[--c->n]; found = 1; break; }
    apx_mutex_unlock(&c->mmu);
    if (!found) return;                            /* not ours (e.g. child_init's own work) */

    /* FINISHED carries status OK + the result; DEAD_LETTERED / DROPPED carry the
     * failure status (E_TASK, E_TIMEOUT, E_DENIED, E_BUDGET, E_SHUTDOWN, ...);
     * FAILED here is only ever the cancelled case. */
    st = (int32_t)(ev->kind == GPTPS_EV_FINISHED ? GPTPS_OK : ev->status);
    apx_mutex_lock(&c->wmu);
    if (ev->kind == GPTPS_EV_FINISHED && (uint64_t)ev->result_len <= GPTPS_XPORT_MAX_MSG)
        write_reply(c->fd, id, st, ev->result, (uint64_t)ev->result_len);
    else
        write_reply(c->fd, id, ev->kind == GPTPS_EV_FINISHED ? (int32_t)GPTPS_E_BUDGET : st, NULL, 0);
    apx_mutex_unlock(&c->wmu);
    /* A failed write means the parent is gone; the main loop will see EOF and shut
     * down, so there is nothing to do here. */
}

static void worker_engine_main(int fd, const gptps_xport_config *cfg)
{
    worker_engine_ctx c;
    gptps *e = NULL;
    size_t i;

    memset(&c, 0, sizeof c);
    c.fd = fd;
    apx_mutex_init(&c.wmu);
    apx_mutex_init(&c.mmu);

    /* A FRESH engine, in this process, after the fork. The parent's engines (if any)
     * are never touched from here. */
    if (gptps_open_ex(cfg->engine_cfg, &e) != GPTPS_OK || !e) { close(fd); _exit(2); }
    for (i = 0; i < cfg->ntasks; ++i)
        if (gptps_register_task(e, &cfg->tasks[i]) != GPTPS_OK) { gptps_shutdown(e); close(fd); _exit(3); }
    if (gptps_set_event_cb(e, worker_engine_ev, &c) != GPTPS_OK) { gptps_shutdown(e); close(fd); _exit(4); }
    if (cfg->child_init) cfg->child_init(e, cfg->child_init_ud);

    for (;;) {
        uint64_t id, plen;
        char *task; void *payload;
        gptps_handle h = 0;
        gptps_status st;

        if (read_request(fd, &id, &task, &payload, &plen) != 0) break;   /* EOF => drain + exit */

        /* Hold mmu ACROSS the submit and the insert: a fast task could otherwise
         * reach its terminal event before its handle is in the table, and the
         * dispatcher would find nothing to answer. The event callback blocks on mmu
         * for that window; it never holds the engine lock while it does, so the
         * submit cannot deadlock on it. */
        apx_mutex_lock(&c.mmu);
        if (c.n == c.cap) {
            size_t nc = c.cap ? c.cap * 2 : 16;
            inflight *nt = (inflight *)realloc(c.tab, nc * sizeof *nt);
            if (nt) { c.tab = nt; c.cap = nc; }
        }
        if (c.n < c.cap) {
            st = gptps_submit(e, task, payload, (size_t)plen, &h);
            if (st == GPTPS_OK) { c.tab[c.n].h = h; c.tab[c.n].id = id; c.n += 1; }
        } else {
            st = GPTPS_E_NOMEM;
        }
        apx_mutex_unlock(&c.mmu);
        free(payload); free(task);                 /* the core copied the payload */

        if (st != GPTPS_OK) {                      /* rejected at submit: answer now */
            int w;
            apx_mutex_lock(&c.wmu);
            w = write_reply(fd, id, (int32_t)st, NULL, 0);
            apx_mutex_unlock(&c.wmu);
            if (w != 0) break;
        }
    }

    /* EOF: the parent closed its request side. Drain what is in flight (bounded by
     * the engine's shutdown grace); every terminal event still owed goes out as a
     * reply through the callback above. */
    gptps_shutdown(e);
    close(fd);
    apx_mutex_destroy(&c.wmu);
    apx_mutex_destroy(&c.mmu);
    free(c.tab);
    _exit(0);
}

/* ============================================================================
 * PARENT
 * ==========================================================================*/
typedef struct pending {
    uint64_t         id;
    struct pending  *next;
    int              is_async;
    /* blocking: lives on the submitter's stack; completed under pmu */
    int              done;
    gptps_status     io;        /* GPTPS_OK: answered; GPTPS_E_IO: link died */
    int32_t          st;
    void            *res;
    uint64_t         rlen;
    /* async: malloc'd; completed on the reader thread, no lock held */
    gptps_xport_reply_fn cb;
    void            *ud;
} pending;

typedef struct {
    pid_t       pid;
    int         fd;              /* parent side of the socketpair; closed in close() */
    apx_mutex   wmu;             /* serialises request frames */
    apx_mutex   pmu;             /* pending list, npend, dead */
    apx_cond    pcv;             /* broadcast on every completion */
    pending    *pend;
    uint32_t    npend;
    int         dead;            /* link retired: reader has failed everything outstanding */
    int         reader_started;
    pthread_t   reader;
} xport_worker;

struct gptps_xport {
    xport_worker  *w;
    size_t         n;
    uint32_t       max_in_flight;
    apx_mutex      cursor_lock;
    uint64_t       cursor;
    uint64_t       next_id;      /* under cursor_lock; unique across links */
    /* Liveness, mirrored under cursor_lock so the rotation can read it without
     * taking a worker's pmu. Lock order: nothing is ever acquired while holding
     * cursor_lock. */
    unsigned char *alive;
    size_t         nalive;
};

static void mark_dead(gptps_xport *xp, size_t idx)
{
    apx_mutex_lock(&xp->cursor_lock);
    if (xp->alive && xp->alive[idx]) { xp->alive[idx] = 0; xp->nalive -= 1; }
    apx_mutex_unlock(&xp->cursor_lock);
}

/* Next LIVE worker (scanning at most n slots), or (size_t)-1 if all are retired. */
static size_t next_rr(gptps_xport *xp)
{
    size_t idx = (size_t)-1, i;
    apx_mutex_lock(&xp->cursor_lock);
    if (xp->nalive) {
        for (i = 0; i < xp->n; ++i) {
            size_t cand = (size_t)(xp->cursor++ % xp->n);
            if (!xp->alive || xp->alive[cand]) { idx = cand; break; }
        }
    }
    apx_mutex_unlock(&xp->cursor_lock);
    return idx;
}
static uint64_t new_id(gptps_xport *xp)
{
    uint64_t id;
    apx_mutex_lock(&xp->cursor_lock);
    id = ++xp->next_id;
    apx_mutex_unlock(&xp->cursor_lock);
    return id;
}

/* Unlink by id. pmu held. */
static pending *pend_take(xport_worker *w, uint64_t id)
{
    pending **pp;
    for (pp = &w->pend; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) { pending *p = *pp; *pp = p->next; p->next = NULL; w->npend -= 1; return p; }
    return NULL;
}

/* Retire a link: everything outstanding fails with E_IO. Runs on the reader thread
 * (after EOF) - which is the only place completions happen, so no completion can
 * race this. */
static void fail_all(gptps_xport *xp, size_t idx)
{
    xport_worker *w = &xp->w[idx];
    pending *p, *async_list = NULL;
    apx_mutex_lock(&w->pmu);
    w->dead = 1;
    /* Publish retirement before a blocking submitter can observe completion.
     * cursor_lock is never held while acquiring pmu, so this order is safe. */
    mark_dead(xp, idx);
    /* Split under the lock. A BLOCKING record lives on its submitter's stack and is
     * gone the moment that thread sees done=1, so it must not be touched after the
     * unlock; only the heap-allocated async records survive to the callback loop. */
    while ((p = w->pend) != NULL) {
        w->pend = p->next;
        if (p->is_async) { p->next = async_list; async_list = p; }
        else             { p->next = NULL; p->io = GPTPS_E_IO; p->done = 1; }
    }
    w->npend = 0;
    apx_cond_broadcast(&w->pcv);
    apx_mutex_unlock(&w->pmu);
    while (async_list) {                            /* callbacks, no lock held */
        pending *n = async_list->next;
        async_list->cb(async_list->id, GPTPS_E_IO, GPTPS_E_IO, NULL, 0, async_list->ud);
        free(async_list);
        async_list = n;
    }
}

typedef struct { gptps_xport *xp; size_t idx; } reader_arg;

static void *reader_main(void *arg)
{
    reader_arg *ra = (reader_arg *)arg;
    gptps_xport *xp = ra->xp; size_t idx = ra->idx;
    xport_worker *w = &xp->w[idx];
    free(ra);

    for (;;) {
        uint64_t id, rl; int32_t st; void *res = NULL; pending *p;
        if (sock_read_all(w->fd, &id, sizeof id) != 0) break;
        if (sock_read_all(w->fd, &st, sizeof st) != 0) break;
        if (sock_read_all(w->fd, &rl, sizeof rl) != 0) break;
        if (rl > GPTPS_XPORT_MAX_MSG) break;          /* bad reply length: the link is not at a frame boundary */
        if (rl) {
            res = malloc((size_t)rl);
            if (!res || sock_read_all(w->fd, res, (size_t)rl) != 0) { free(res); break; }
        }
        apx_mutex_lock(&w->pmu);
        p = pend_take(w, id);
        if (p && !p->is_async) {                      /* complete the waiter under pmu */
            p->st = st; p->res = res; p->rlen = rl; p->io = GPTPS_OK; p->done = 1;
            apx_cond_broadcast(&w->pcv);
            res = NULL;
            p = NULL;                                 /* its stack may be gone after the unlock */
        }
        apx_mutex_unlock(&w->pmu);
        if (p) {                                      /* async: callback with no lock held */
            p->cb(p->id, GPTPS_OK, (gptps_status)st, res, (size_t)rl, p->ud);
            free(p);
        }
        free(res);                                    /* unknown id (should not happen): drop */
    }
    /* EOF or a broken frame. Wake a writer that may be blocked on a dead socket and
     * make sure nobody speaks to this link again, then fail what is outstanding. */
    shutdown(w->fd, SHUT_RDWR);
    fail_all(xp, idx);
    return NULL;
}

gptps_xport *gptps_xport_open_ex(const gptps_xport_config *cfg)
{
    gptps_xport *xp;
    size_t i;
    int engine_mode;

    if (!cfg || cfg->struct_size < sizeof(gptps_xport_config) || cfg->nworkers < 1) return NULL;
    if (cfg->run && cfg->tasks) return NULL;            /* pick a mode */
    if (!cfg->run && !cfg->tasks) return NULL;
    engine_mode = (cfg->tasks != NULL);

    xp = (gptps_xport *)calloc(1, sizeof *xp);
    if (!xp) return NULL;
    xp->w = (xport_worker *)calloc(cfg->nworkers, sizeof *xp->w);
    xp->alive = (unsigned char *)calloc(cfg->nworkers, 1);
    if (!xp->w || !xp->alive) { free(xp->w); free(xp->alive); free(xp); return NULL; }
    xp->max_in_flight = cfg->max_in_flight ? cfg->max_in_flight : GPTPS_XPORT_DEFAULT_IN_FLIGHT;
    apx_mutex_init(&xp->cursor_lock);

    /* Phase 1: fork every worker. No xport thread exists yet, so each child is born
     * from a parent that holds none of this module's locks. */
    for (i = 0; i < cfg->nworkers; ++i) {
        int sp[2];
        pid_t pid;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) { gptps_xport_close(xp); return NULL; }
        set_nosigpipe(sp[0]); set_nosigpipe(sp[1]);
        pid = fork();
        if (pid < 0) { close(sp[0]); close(sp[1]); gptps_xport_close(xp); return NULL; }
        if (pid == 0) {
            /* CHILD i: keep only sp[1]; drop this pair's parent end and every prior
             * worker's parent fd we inherited, so no stray fd holds a link open. */
            size_t j;
            close(sp[0]);
            for (j = 0; j < i; ++j) close(xp->w[j].fd);
            if (engine_mode) worker_engine_main(sp[1], cfg);            /* never returns */
            else             worker_handler_main(sp[1], cfg->run, cfg->user_data);
        }
        /* PARENT */
        close(sp[1]);
        xp->w[i].pid = pid; xp->w[i].fd = sp[0];
        apx_mutex_init(&xp->w[i].wmu);
        apx_mutex_init(&xp->w[i].pmu);
        apx_cond_init(&xp->w[i].pcv);
        xp->alive[i] = 1; xp->nalive = i + 1;
        xp->n = i + 1;                                   /* keep n consistent for a partial teardown */
    }

    /* Phase 2: one reader per link. */
    for (i = 0; i < xp->n; ++i) {
        reader_arg *ra = (reader_arg *)malloc(sizeof *ra);
        if (!ra) { gptps_xport_close(xp); return NULL; }
        ra->xp = xp; ra->idx = i;
        if (pthread_create(&xp->w[i].reader, NULL, reader_main, ra) != 0) {
            free(ra); gptps_xport_close(xp); return NULL;
        }
        xp->w[i].reader_started = 1;
    }
    return xp;
}

gptps_xport *gptps_xport_open(size_t nworkers, gptps_xport_run_fn run, void *user_data)
{
    gptps_xport_config cfg;
    if (!run) return NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.nworkers = nworkers;
    cfg.run = run; cfg.user_data = user_data;
    return gptps_xport_open_ex(&cfg);
}

size_t gptps_xport_count(gptps_xport *xp) { return xp ? xp->n : 0; }

size_t gptps_xport_live(gptps_xport *xp)
{
    size_t n;
    if (!xp) return 0;
    apx_mutex_lock(&xp->cursor_lock);
    n = xp->nalive;
    apx_mutex_unlock(&xp->cursor_lock);
    return n;
}

size_t gptps_xport_in_flight(gptps_xport *xp)
{
    size_t i, n = 0;
    if (!xp) return 0;
    for (i = 0; i < xp->n; ++i) {
        apx_mutex_lock(&xp->w[i].pmu);
        n += xp->w[i].npend;
        apx_mutex_unlock(&xp->w[i].pmu);
    }
    return n;
}

/* Common path: validate, pick a worker, register `p` (bounded), write the frame.
 * On return GPTPS_OK, `p` is on the wire and owned by the link until completed. */
static gptps_status send_request(gptps_xport *xp, const char *task, const void *payload,
                                 size_t len, pending *p, xport_worker **out_w, uint64_t *out_id)
{
    xport_worker *w;
    size_t tl, wi;
    uint32_t tlen;
    uint64_t plen;
    int ok;

    if (!xp || !task) return GPTPS_E_INVAL;
    if (len && !payload) return GPTPS_E_INVAL;   /* would send() from a NULL buffer */
    /* Enforce the cap on the SENDING side: an argument the caller can be told about
     * locally must not cost a worker for the lifetime of the transport. */
    tl = strlen(task);
    if ((uint64_t)tl > GPTPS_XPORT_MAX_MSG) return GPTPS_E_INVAL;
    if ((uint64_t)len > GPTPS_XPORT_MAX_MSG) return GPTPS_E_INVAL;
    tlen = (uint32_t)tl;
    plen = (uint64_t)len;

    wi = next_rr(xp);
    if (wi == (size_t)-1) return GPTPS_E_IO;      /* every worker has been retired */
    w = &xp->w[wi];
    p->id = new_id(xp);
    *out_id = p->id;        /* copy out NOW: once registered, `p` may be completed and
                             * (if async) freed by the reader before we return */

    /* Register BEFORE writing: the reply can only be matched to a record that
     * already exists, and a fast worker can answer before write returns. */
    apx_mutex_lock(&w->pmu);
    if (w->dead) { apx_mutex_unlock(&w->pmu); return GPTPS_E_IO; }
    if (w->npend >= xp->max_in_flight) { apx_mutex_unlock(&w->pmu); return GPTPS_E_FULL; }
    p->next = w->pend; w->pend = p; w->npend += 1;
    apx_mutex_unlock(&w->pmu);

    apx_mutex_lock(&w->wmu);
    ok = sock_write_all(w->fd, &p->id, sizeof p->id) == 0 &&
         sock_write_all(w->fd, &tlen, sizeof tlen) == 0 &&
         sock_write_all(w->fd, task, tlen) == 0 &&
         sock_write_all(w->fd, &plen, sizeof plen) == 0 &&
         (len == 0 || sock_write_all(w->fd, payload, len) == 0);
    if (!ok) shutdown(w->fd, SHUT_RDWR);          /* half a frame is on the wire: retire the link */
    apx_mutex_unlock(&w->wmu);

    if (!ok) {
        /* Take our record back if the reader has not already failed it. The reader
         * sees the shutdown as EOF and retires the link for everyone else. */
        apx_mutex_lock(&w->pmu);
        if (!p->done) pend_take(w, p->id);
        apx_mutex_unlock(&w->pmu);
        return GPTPS_E_IO;
    }
    *out_w = w;
    return GPTPS_OK;
}

gptps_status gptps_xport_submit(gptps_xport *xp, const char *task,
                                const void *payload, size_t len,
                                void **out_result, size_t *out_len,
                                gptps_status *out_task_status)
{
    pending p;
    xport_worker *w = NULL;
    gptps_status st;
    uint64_t id;

    if (out_result) *out_result = NULL;
    if (out_len) *out_len = 0;
    if (out_task_status) *out_task_status = GPTPS_E_IO;

    memset(&p, 0, sizeof p);
    st = send_request(xp, task, payload, len, &p, &w, &id);
    if (st != GPTPS_OK) return st;

    apx_mutex_lock(&w->pmu);
    while (!p.done) apx_cond_wait_ms(&w->pcv, &w->pmu, 1000u);   /* periodic re-check only; no timeout */
    apx_mutex_unlock(&w->pmu);

    if (p.io != GPTPS_OK) { free(p.res); return GPTPS_E_IO; }
    if (out_task_status) *out_task_status = (gptps_status)p.st;
    if (out_result) *out_result = p.res; else free(p.res);
    if (out_len) *out_len = (size_t)p.rlen;
    return GPTPS_OK;
}

gptps_status gptps_xport_submit_async(gptps_xport *xp, const char *task,
                                      const void *payload, size_t len,
                                      gptps_xport_reply_fn cb, void *user_data,
                                      uint64_t *out_request_id)
{
    pending *p;
    xport_worker *w = NULL;
    gptps_status st;
    uint64_t id;

    if (out_request_id) *out_request_id = 0;
    if (!cb) return GPTPS_E_INVAL;
    p = (pending *)calloc(1, sizeof *p);
    if (!p) return GPTPS_E_NOMEM;
    p->is_async = 1; p->cb = cb; p->ud = user_data;
    st = send_request(xp, task, payload, len, p, &w, &id);
    if (st != GPTPS_OK) { free(p); return st; }
    /* Do not touch `p` from here: the reader may already have completed and freed it. */
    if (out_request_id) *out_request_id = id;
    return GPTPS_OK;
}

void gptps_xport_close(gptps_xport *xp)
{
    size_t i;
    if (!xp) return;
    /* Phase 1: close the REQUEST side of every link. A worker sees EOF, drains
     * (engine mode: gptps_shutdown, bounded by its grace), sends what it still owes,
     * and exits. Retired links are already shut. */
    for (i = 0; i < xp->n; ++i) {
        apx_mutex_lock(&xp->w[i].wmu);
        if (xp->w[i].fd >= 0) shutdown(xp->w[i].fd, SHUT_WR);
        apx_mutex_unlock(&xp->w[i].wmu);
    }
    /* Phase 2: readers run until the worker's EOF, completing the last replies. */
    for (i = 0; i < xp->n; ++i)
        if (xp->w[i].reader_started) pthread_join(xp->w[i].reader, NULL);
    /* Phase 3: reap and free. */
    for (i = 0; i < xp->n; ++i) {
        int st;
        if (xp->w[i].fd >= 0) close(xp->w[i].fd);
        while (waitpid(xp->w[i].pid, &st, 0) < 0 && errno == EINTR) { }
        apx_mutex_destroy(&xp->w[i].wmu);
        apx_mutex_destroy(&xp->w[i].pmu);
        apx_cond_destroy(&xp->w[i].pcv);
    }
    apx_mutex_destroy(&xp->cursor_lock);
    free(xp->alive);
    free(xp->w);
    free(xp);
}

#endif /* !_WIN32 */
