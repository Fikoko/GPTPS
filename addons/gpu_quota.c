/*
 * gpu_quota.c - GPU-unit admission quota (see gpu_quota.h).
 *
 * Constraint (dispatcher thread): reserve gpu_units on ADMIT, DEFER when full.
 * Observer (worker/dispatcher thread): release on a run-end event (FINISHED or
 * FAILED - each marks the end of one attempt; retries re-admit and re-reserve).
 * Both share one counter + a name->units table under a mutex. Because the engine
 * admits a `best` candidate the instant its sole constraint returns ADMIT, every
 * reserve corresponds to an admitted task that will emit exactly one run-end
 * event - so reserve/release stays balanced (install as the GPU constraint).
 *
 * The constraint sees (name, cost) but no handle; the observer sees (name,
 * handle) but no cost - so we learn name->units from the constraint and look it
 * up by name on release, assuming gpu_units is fixed per task type.
 */
#define _POSIX_C_SOURCE 200809L
#include "gpu_quota.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct { char *name; uint64_t units; } gpu_entry;

struct gptps_gpu_quota {
    gptps          *e;
    pthread_mutex_t mu;
    uint64_t        budget;
    uint64_t        reserved;
    uint32_t        defer_ms;
    gpu_entry      *map;
    size_t          n, cap;
};

static char *dup_str(const char *s) { size_t n = strlen(s) + 1; char *o = (char *)malloc(n); if (o) memcpy(o, s, n); return o; }

/* record name->units (fixed per type); caller holds mu */
static void remember(gptps_gpu_quota *q, const char *name, uint64_t units)
{
    size_t i;
    for (i = 0; i < q->n; ++i)
        if (strcmp(q->map[i].name, name) == 0) { q->map[i].units = units; return; }
    if (q->n == q->cap) {
        size_t nc = q->cap ? q->cap * 2 : 8;
        gpu_entry *nm = (gpu_entry *)realloc(q->map, nc * sizeof *nm);
        if (!nm) return;            /* drop the hint; release will just no-op */
        q->map = nm; q->cap = nc;
    }
    q->map[q->n].name = dup_str(name);
    q->map[q->n].units = units;
    if (q->map[q->n].name) q->n += 1;
}

/* caller holds mu */
static uint64_t lookup(gptps_gpu_quota *q, const char *name)
{
    size_t i;
    for (i = 0; i < q->n; ++i)
        if (strcmp(q->map[i].name, name) == 0) return q->map[i].units;
    return 0;
}

static gptps_admit_decision gpu_gate(const char *name, const gptps_cost *cost,
                                     uint32_t *retry_after_ms, void *ud)
{
    gptps_gpu_quota *q = (gptps_gpu_quota *)ud;
    uint64_t need = cost ? cost->gpu_units : 0;
    gptps_admit_decision dec;
    if (need == 0) return GPTPS_ADMIT;          /* non-GPU work isn't gated */
    pthread_mutex_lock(&q->mu);
    remember(q, name, need);
    if (q->reserved + need <= q->budget) {
        q->reserved += need;
        dec = GPTPS_ADMIT;
    } else {
        *retry_after_ms = q->defer_ms;
        dec = GPTPS_DEFER;
    }
    pthread_mutex_unlock(&q->mu);
    return dec;
}

static void gpu_release(const gptps_event *ev, void *ud)
{
    gptps_gpu_quota *q = (gptps_gpu_quota *)ud;
    uint64_t units;
    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_FAILED) return;
    pthread_mutex_lock(&q->mu);
    units = lookup(q, ev->task_name);
    if (units) q->reserved = (q->reserved >= units) ? q->reserved - units : 0;
    pthread_mutex_unlock(&q->mu);
}

gptps_gpu_quota *gptps_gpu_quota_install(gptps *e, uint64_t total_units, uint32_t defer_ms)
{
    gptps_gpu_quota *q;
    if (!e) return NULL;
    q = (gptps_gpu_quota *)calloc(1, sizeof *q);
    if (!q) return NULL;
    pthread_mutex_init(&q->mu, NULL);
    q->e = e; q->budget = total_units; q->defer_ms = defer_ms ? defer_ms : 10u;

    if (gptps_register_constraint(e, gpu_gate, q) != GPTPS_OK) {
        pthread_mutex_destroy(&q->mu); free(q);   /* nothing registered: safe to free */
        return NULL;
    }
    if (gptps_register_observer(e, gpu_release, q) != GPTPS_OK) {
        /* the constraint now references q; freeing it would dangle. Leak q (only
         * reachable on an allocation failure) rather than risk a use-after-free. */
        return NULL;
    }
    return q;
}

uint64_t gptps_gpu_quota_in_use(gptps_gpu_quota *q)
{
    uint64_t v;
    if (!q) return 0;
    pthread_mutex_lock(&q->mu);
    v = q->reserved;
    pthread_mutex_unlock(&q->mu);
    return v;
}

uint64_t gptps_gpu_quota_total(gptps_gpu_quota *q) { return q ? q->budget : 0; }

void gptps_gpu_quota_close(gptps_gpu_quota *q)
{
    size_t i;
    if (!q) return;
    /* Caller contract: engine already shut down, so no constraint/observer fires. */
    for (i = 0; i < q->n; ++i) free(q->map[i].name);
    free(q->map);
    pthread_mutex_destroy(&q->mu);
    free(q);
}
