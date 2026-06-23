/*
 * gptps_orch.c - run-after / fan-in orchestration (see gptps_orch.h).
 *
 * One observer watches terminal events and remembers completed handles. A gate
 * holds a pending submission plus a count of unsatisfied dependencies; when that
 * count reaches zero the gate is released (gptps_submit). Both the public
 * gptps_orch_after path and the observer take the same lock, in the same order
 * (orch lock -> engine lock, via gptps_submit), so there is no lock inversion;
 * the observer runs with the engine lock released, which the API guarantees.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_orch.h"
#include "addon_compat.h"   /* portable mutex */

#include <stdlib.h>
#include <string.h>

typedef struct {
    char         *task;
    void         *payload;
    size_t        len;
    gptps_handle *deps;
    size_t        ndeps;
    size_t        remaining;   /* deps not yet terminal */
    int           released;
} gate;

struct gptps_orch {
    gptps        *e;
    apx_mutex     mu;
    gate         *gates;
    size_t        ng, gcap;
    gptps_handle *done;        /* terminal handles seen so far */
    size_t        ndone, dcap;
};

static char *dup_str(const char *s) { size_t n = strlen(s) + 1; char *o = (char *)malloc(n); if (o) memcpy(o, s, n); return o; }
static void *dup_mem(const void *s, size_t n) { void *o; if (!n) return NULL; o = malloc(n); if (o) memcpy(o, s, n); return o; }

static int is_done(gptps_orch *o, gptps_handle h)
{
    size_t i;
    for (i = 0; i < o->ndone; ++i) if (o->done[i] == h) return 1;
    return 0;
}

static void mark_done(gptps_orch *o, gptps_handle h)
{
    if (is_done(o, h)) return;
    if (o->ndone == o->dcap) {
        size_t nc = o->dcap ? o->dcap * 2 : 16;
        gptps_handle *nd = (gptps_handle *)realloc(o->done, nc * sizeof *nd);
        if (!nd) return;             /* drop the hint; a gate on this handle may stall */
        o->done = nd; o->dcap = nc;
    }
    o->done[o->ndone++] = h;
}

/* release a gate: submit its task (caller holds o->mu; gptps_submit takes the
 * engine lock, preserving the orch->engine lock order). */
static void release_gate(gptps_orch *o, gate *g)
{
    gptps_handle h = 0;
    g->released = 1;
    gptps_submit(o->e, g->task, g->payload, g->len, &h);
    /* the released task is now a normal engine item; we don't track its handle
     * further (chaining onto a deferred gate is out of scope - see the header). */
}

static void orch_obs(const gptps_event *ev, void *ud)
{
    gptps_orch *o = (gptps_orch *)ud;
    size_t i, j;
    /* only terminal events advance dependencies; ignore QUEUED/STARTED/RETRIED
     * (and return before taking the lock so a re-entrant QUEUED can't deadlock). */
    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_FAILED &&
        ev->kind != GPTPS_EV_DROPPED && ev->kind != GPTPS_EV_DEAD_LETTERED) return;

    apx_mutex_lock(&o->mu);
    mark_done(o, ev->handle);
    for (i = 0; i < o->ng; ++i) {
        gate *g = &o->gates[i];
        if (g->released) continue;
        for (j = 0; j < g->ndeps; ++j) {
            if (g->deps[j] == ev->handle && g->remaining > 0) { g->remaining -= 1; break; }
        }
        if (g->remaining == 0) release_gate(o, g);
    }
    apx_mutex_unlock(&o->mu);
}

gptps_orch *gptps_orch_install(gptps *e)
{
    gptps_orch *o;
    if (!e) return NULL;
    o = (gptps_orch *)calloc(1, sizeof *o);
    if (!o) return NULL;
    apx_mutex_init(&o->mu);
    o->e = e;
    if (gptps_register_observer(e, orch_obs, o) != GPTPS_OK) {
        apx_mutex_destroy(&o->mu); free(o); return NULL;
    }
    return o;
}

gptps_status gptps_orch_after(gptps_orch *o, const char *task,
                              const void *payload, size_t len,
                              const gptps_handle *deps, size_t ndeps,
                              gptps_handle *out)
{
    gate *g;
    size_t i, remaining;
    if (out) *out = 0;
    if (!o || !task || (ndeps && !deps)) return GPTPS_E_INVAL;

    apx_mutex_lock(&o->mu);

    /* fast path: no deps, or all deps already terminal => submit immediately */
    remaining = 0;
    for (i = 0; i < ndeps; ++i) if (!is_done(o, deps[i])) ++remaining;
    if (remaining == 0) {
        gptps_handle h = 0;
        gptps_status st = gptps_submit(o->e, task, payload, len, &h);
        apx_mutex_unlock(&o->mu);
        if (st == GPTPS_OK && out) *out = h;
        return st;
    }

    /* hold a gate until the remaining deps finish */
    if (o->ng == o->gcap) {
        size_t nc = o->gcap ? o->gcap * 2 : 8;
        gate *ng = (gate *)realloc(o->gates, nc * sizeof *ng);
        if (!ng) { apx_mutex_unlock(&o->mu); return GPTPS_E_NOMEM; }
        o->gates = ng; o->gcap = nc;
    }
    g = &o->gates[o->ng];
    memset(g, 0, sizeof *g);
    g->task = dup_str(task);
    g->payload = dup_mem(payload, len);
    g->len = len;
    g->ndeps = ndeps;
    g->remaining = remaining;
    g->deps = (gptps_handle *)malloc(ndeps * sizeof *g->deps);
    if (!g->task || (len && !g->payload) || !g->deps) {
        free(g->task); free(g->payload); free(g->deps);
        apx_mutex_unlock(&o->mu);
        return GPTPS_E_NOMEM;
    }
    memcpy(g->deps, deps, ndeps * sizeof *g->deps);
    o->ng += 1;
    apx_mutex_unlock(&o->mu);
    return GPTPS_OK;
}

size_t gptps_orch_pending(gptps_orch *o)
{
    size_t i, n = 0;
    if (!o) return 0;
    apx_mutex_lock(&o->mu);
    for (i = 0; i < o->ng; ++i) if (!o->gates[i].released) ++n;
    apx_mutex_unlock(&o->mu);
    return n;
}

void gptps_orch_close(gptps_orch *o)
{
    size_t i;
    if (!o) return;
    /* Caller contract: engine already shut down, so no observer fires here. */
    for (i = 0; i < o->ng; ++i) { free(o->gates[i].task); free(o->gates[i].payload); free(o->gates[i].deps); }
    free(o->gates); free(o->done);
    apx_mutex_destroy(&o->mu);
    free(o);
}
