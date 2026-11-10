/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_adapt.h - memory declarations from evidence (add-on).
 *
 * Admission budgets what a task DECLARES (cost.mem_bytes), and the process executors
 * cap a job at it. Since ABI 2.5 every process job also reports what it USED
 * (mem.peak, docs/MEASUREMENTS.md). The core reports and never acts - a measurement
 * never changes a declaration - because acting is policy, and the policy here has
 * knobs a mechanism should not pick: how much headroom, after how many runs, how hard
 * to grow after a job hit its cap, and whether to change anything without being told.
 *
 * This add-on is that policy, on the observer seam. Per task type it keeps the largest
 * peak seen and whether a job hit its cap, and turns them into a suggested mem_bytes:
 *
 *   no cap hit:  largest peak * (100 + headroom_pct) / 100, once min_samples attempts
 *                have been measured
 *   a cap hit:   at least the declaration that was hit * (100 + grow_pct) / 100, at
 *                once - a job that hits its cap fails every time, so waiting for more
 *                samples only fails more of them. It never goes back below that: the
 *                job needed MORE than the declaration it hit.
 *
 * rounded up to a whole MiB and raised to floor_bytes. It goes DOWN as well as up: a
 * task declared at 24 GB that peaks at 19 GB is suggested ~24 GB at 25% headroom, and
 * one declared at 4 GB that peaks at 1 GB is suggested 1.25 GB, which admits four
 * times as many beside it.
 *
 * Nothing changes unless you say so:
 *   gptps_adapt_get(a, "mrp", &r)   read the evidence and the suggestion
 *   gptps_adapt_apply(a, "mrp")     write it to tasks.mrp.mem_bytes (a live setting,
 *                                   so it is saved by gptps_settings_save like any)
 *   cfg.auto_apply = 1              do that as suggestions change
 *
 * WHAT IT COUNTS. A cap hit is a mem.cap_hit of 1, or a process job that FAILED with
 * GPTPS_E_NOMEM (how RLIMIT_AS and a task that ran out report it). Only events that
 * carry measurements count, so an in-process task - which has none - is never
 * adapted. MEASUREMENTS.md rule 4: never mix methods. The first mem.peak method seen
 * for a task is the one its evidence is kept in; a peak measured another way (the
 * job moved into cgroup mode, say) is counted in `other_method` and not folded in.
 *
 * WHAT IT CHANGES. Only the task type's default declaration. A task with a cost()
 * function declares per item from its payload, and that function wins over the
 * setting - adapt it yourself from the report, if at all.
 *
 *   a = gptps_adapt_open(e, NULL);     // BEFORE submitting, so no evidence is missed
 *   ...
 *   gptps_shutdown(e);
 *   gptps_adapt_close(a);              // AFTER shutdown
 *
 * Portable: public C99 API plus addon_compat.h's mutex. No core change.
 */
#ifndef GPTPS_ADAPT_H
#define GPTPS_ADAPT_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_adapt gptps_adapt;

typedef struct {
    size_t   struct_size;     /* = sizeof(gptps_adapt_config) */
    uint32_t headroom_pct;    /* above the largest peak; 0 = the default, 25 */
    uint32_t min_samples;     /* measured attempts before a suggestion; 0 = the default, 5 */
    uint32_t grow_pct;        /* above a declaration that was hit; 0 = the default, 50 */
    uint64_t floor_bytes;     /* never suggest less; 0 = no floor */
    int      auto_apply;      /* 1: apply each changed suggestion as it is made */
} gptps_adapt_config;

typedef struct {
    size_t   struct_size;     /* = sizeof(gptps_adapt_report) */
    uint64_t declared;        /* the declaration on the latest measured attempt */
    uint64_t peak;            /* the largest mem.peak in `method` */
    uint64_t samples;         /* measured attempts counted */
    uint64_t cap_hits;        /* of those, how many hit their cap */
    uint64_t hit_declared;    /* the largest declaration that was hit; 0 = none */
    uint64_t other_method;    /* peaks measured another way, not counted */
    uint64_t suggested;       /* 0 = not enough evidence yet */
    uint64_t applied;         /* times a suggestion was written to the setting */
    char     method[48];      /* the mem.peak method the evidence is in ("" = none yet) */
} gptps_adapt_report;

/* Start watching an engine. `cfg` may be NULL for the defaults. NULL on allocation
 * failure or if the observer could not be registered. */
gptps_adapt *gptps_adapt_open(gptps *e, const gptps_adapt_config *cfg);

/* The evidence and the suggestion for one task type. GPTPS_E_NOTFOUND if no measured
 * attempt of it has been seen; GPTPS_E_INVAL for a NULL argument or an `out` whose
 * struct_size is below sizeof(gptps_adapt_report). */
gptps_status gptps_adapt_get(gptps_adapt *a, const char *task, gptps_adapt_report *out);

/* Write the suggestion to tasks.<task>.mem_bytes. GPTPS_E_NOTFOUND as above,
 * GPTPS_E_BUSY when there is no suggestion yet (too few samples), or what the setting
 * write returned. Not from inside a task body; from an event callback it is fine. */
gptps_status gptps_adapt_apply(gptps_adapt *a, const char *task);

/* Free it. Call AFTER gptps_shutdown. NULL is a no-op. */
void gptps_adapt_close(gptps_adapt *a);

#ifdef __cplusplus
}
#endif

#endif /* GPTPS_ADAPT_H */
