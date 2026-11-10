/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_sched.h - ready-made admission disciplines (add-on).
 *
 * The core's scheduler seam (gptps_set_scheduler) has always said "deadline-first,
 * per-tenant fair-share, cost-aware, aging ... compose without a core fork". Saying
 * so is not the same as shipping one, and an ordering key is easy to get subtly
 * wrong - a score that jumps around weakens the starvation guard, a fair-share that
 * charges at the wrong moment hands one tenant every free slot. These are the three
 * disciplines people ask for, written once against the public seams, plus the one
 * policy a deadline is usually wanted for:
 *
 *   gptps_sched_edf        earliest deadline first, over the due time a submit gives
 *                          with GPTPS_SUBMIT_DEADLINE (ABI 2.6)
 *   gptps_sched_aging      priority, with waiting worth priority: nothing starves
 *   gptps_sched_fair       weighted fair share between tenants (start-time fair
 *                          queuing), tenants named by a function of the item
 *   gptps_sched_shed_late  a CONSTRAINT, not a scheduler: work already past its due
 *                          time (plus a grace) is refused - dead-lettered with
 *                          GPTPS_E_DENIED - instead of run late
 *
 * The first three each OWN the scheduler seam, which holds exactly one (see
 * gptps_set_scheduler_ex: an ordering is a total order, and two do not compose). So
 * they install with flags == 0 and fail with GPTPS_E_BUSY rather than replace an
 * ordering someone else installed - the rule the core sets for add-ons. shed_late is
 * a constraint, which is many-per-engine, so it composes with any of them, or with a
 * scheduler of your own.
 *
 * None of these changes the MECHANISM: an item still runs only when it fits the
 * live budget, a too-large one is still skipped for smaller work behind it, and the
 * starvation guard still reserves for one that is skipped too often. They change only
 * which pending item is "best".
 *
 *   gptps_sched *s;
 *   gptps_sched_edf(e, &s);                       // setup time, before submitting
 *   gptps_submit_options o = { sizeof o };
 *   o.flags = GPTPS_SUBMIT_DEADLINE; o.deadline_in_ms = 250;
 *   gptps_submit_ex(e, "frame", buf, len, &o, &h);
 *   ...
 *   gptps_shutdown(e);
 *   gptps_sched_close(s);                         // AFTER shutdown
 *
 * LIFETIME. Install at setup time, while the engine is quiescent, as the seams
 * require. Close AFTER gptps_shutdown: the engine calls into the add-on until it is
 * gone, and gptps_sched_close does not call the engine (which may be gone by then).
 *
 * Portable: public C99 API plus addon_compat.h's mutex. No core change.
 */
#ifndef GPTPS_SCHED_H
#define GPTPS_SCHED_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_sched gptps_sched;

/* EARLIEST DEADLINE FIRST. An item with a due time (GPTPS_SUBMIT_DEADLINE) goes ahead
 * of every item without one, the earliest due first; items without one follow, by
 * priority (higher first). Ties go to whichever was queued first. The key never moves
 * while an item waits, so the starvation guard keeps its per-item bound.
 * GPTPS_E_BUSY if the scheduler seam is taken. */
gptps_status gptps_sched_edf(gptps *e, gptps_sched **out);

/* PRIORITY WITH AGING. Every `ms_per_level` milliseconds an item has waited count as
 * one level of priority: an item of priority p+1 goes ahead of one of priority p
 * queued less than ms_per_level ms before it, and behind one queued earlier. So low
 * priority work is delayed, never starved. 0 = by age alone (FIFO across priorities).
 * The score is priority * ms_per_level - enqueue time, which is the same ordering as
 * "priority + age" but does not change from pass to pass. */
gptps_status gptps_sched_aging(gptps *e, uint32_t ms_per_level, gptps_sched **out);

/* WEIGHTED FAIR SHARE between tenants. `tenant` names the tenant of an item - from its
 * payload, its task name, anything in the input; NULL uses the task name, so every
 * task type is a tenant. Admissions are shared between the tenants that have work
 * waiting in proportion to their weights (gptps_sched_fair_weight; 1 by default): with
 * two tenants of weight 2 and 1 both busy, two of every three admissions go to the
 * first. A tenant that was idle comes back at par - it does not bank the turns it did
 * not take - and ties go FIFO.
 *
 * How: start-time fair queuing. Each item is given a virtual start time when it is
 * first seen, max(its tenant's last finish, the virtual time now), and its tenant's
 * finish moves on by 1/weight; the earliest start goes first. Tags are per ITEM, so a
 * pass that admits several items at once still interleaves tenants rather than giving
 * them all to one. Virtual time advances as items START, which this add-on learns from
 * an observer it registers. A share is a share of ADMISSIONS, not of CPU time: a
 * tenant whose items run ten times longer gets ten times the machine. Weight by cost
 * yourself (a tenant per cost class) if that matters.
 *
 * `max_tenants` bounds the tenant table (0 = 64). Past it, further tenants share one
 * slot - and one share - between them; size it for the tenants you have. Items are
 * tracked only while they wait, so that table grows with the queue, not with time.
 * GPTPS_E_BUSY if the scheduler seam is taken, GPTPS_E_NOMEM, or whatever registering
 * the observer returned. */
typedef uint64_t (*gptps_sched_tenant_fn)(const gptps_sched_input *in, void *user_data);
gptps_status gptps_sched_fair(gptps *e, gptps_sched_tenant_fn tenant, void *user_data,
                              size_t max_tenants, gptps_sched **out);

/* Set a tenant's weight (1..1048576), for items scored from now on. GPTPS_E_INVAL for a
 * weight out of range or a handle that is not a fair-share one. Callable at any time. */
gptps_status gptps_sched_fair_weight(gptps_sched *s, uint64_t tenant, uint32_t weight);

/* SHED LATE WORK (a constraint). An item whose due time passed more than `grace_ms`
 * ago when it comes up for admission is refused: dead-lettered with GPTPS_E_DENIED,
 * where gptps_dead_letter_drain can still reach it. Items without a due time are
 * never refused. Composes with any scheduler, these included. */
gptps_status gptps_sched_shed_late(gptps *e, uint32_t grace_ms, gptps_sched **out);

/* How many items shed_late refused so far (0 for any other kind). */
uint64_t gptps_sched_shed_count(const gptps_sched *s);

/* Free it. Call AFTER gptps_shutdown (see LIFETIME). NULL is a no-op. */
void gptps_sched_close(gptps_sched *s);

#ifdef __cplusplus
}
#endif

#endif /* GPTPS_SCHED_H */
