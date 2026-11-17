/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_retry.c - retry rules by exit code (see gptps_retry.h).
 *
 * The observer runs on whichever thread emits, with the engine lock released; the
 * constraint runs on the dispatcher with the engine lock held. Both take r->mu, a
 * LEAF: neither calls the engine while holding it.
 *
 * A FAILED attempt's verdict is stored by handle until the item's next admission
 * consumes it. The order is guaranteed: an attempt's FAILED is emitted by the thread
 * that ran it, before the item goes back to the dispatcher - so before any retry of
 * it can be admitted. A verdict whose retry never comes (no retries left, cancelled
 * while it waited) is forgotten on the item's terminal event, and the table is
 * bounded besides: past MAX_PENDING the oldest verdict is dropped, and that retry
 * runs as its policy says - the safe direction to be wrong in.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* addon_compat.h: fileno, ftruncate */
#endif
#include "gptps_retry.h"
#include "addon_compat.h"   /* portable mutex */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define NAME_MAX_LEN 128
#define MAX_PENDING  4096

typedef struct {
    char     task[NAME_MAX_LEN + 1];   /* "" = every task type */
    int      any;
    int      start;                    /* a rule for start failures (start.error), not a code */
    uint32_t code;
    uint32_t wait_ms;
} rule;

typedef struct {
    gptps_handle handle;
    uint32_t     wait_ms;     /* GPTPS_RETRY_NEVER, or how long to hold the retry */
    uint64_t     until_ms;    /* 0 until its admission is first held */
} verdict;

struct gptps_retry {
    apx_mutex mu;
    rule     *rules;
    size_t    nrules, caprules;
    verdict  *pend;           /* oldest first */
    size_t    npend, cappend;
    uint64_t  refused, delayed, forgotten;
    int       forgot_said;    /* the one WARN about a forgotten verdict is out */
};

/* the rule for (task, code) - or for a start failure, with start set: a task's own
 * first, then an all-tasks one; NULL = none */
static const rule *find_rule(const gptps_retry *r, const char *task, int start, uint32_t code)
{
    const rule *any = NULL;
    size_t i;
    for (i = 0; i < r->nrules; ++i) {
        const rule *x = &r->rules[i];
        if (x->start != start || (!start && x->code != code)) continue;
        if (x->any) { any = x; continue; }
        if (task && strcmp(x->task, task) == 0) return x;
    }
    return any;
}

static size_t find_pend(const gptps_retry *r, gptps_handle h)
{
    size_t i;
    for (i = r->npend; i-- > 0;) if (r->pend[i].handle == h) return i;
    return (size_t)-1;
}

static void drop_pend(gptps_retry *r, size_t i)
{
    memmove(&r->pend[i], &r->pend[i + 1], (r->npend - i - 1) * sizeof *r->pend);
    r->npend -= 1;
}

/* Returns 1 when a verdict had to be forgotten to make room (never silently: the
 * caller counts it, and says so once, outside the lock). */
static int put_pend(gptps_retry *r, gptps_handle h, uint32_t wait_ms)
{
    size_t i = find_pend(r, h);
    int forgot = 0;
    if (i != (size_t)-1) drop_pend(r, i);
    if (r->npend == r->cappend) {
        size_t ncap = r->cappend ? r->cappend * 2 : 16;
        verdict *np;
        if (ncap > MAX_PENDING) ncap = MAX_PENDING;
        if (ncap == r->cappend) { drop_pend(r, 0); forgot = 1; }           /* full: forget the oldest */
        else if ((np = (verdict *)realloc(r->pend, ncap * sizeof *np)) != NULL) { r->pend = np; r->cappend = ncap; }
        else if (r->npend) { drop_pend(r, 0); forgot = 1; }                /* out of memory: make room */
        else { r->forgotten += 1; return 1; }                              /* nowhere to keep this one */
    }
    r->pend[r->npend].handle = h;
    r->pend[r->npend].wait_ms = wait_ms;
    r->pend[r->npend].until_ms = 0;
    r->npend += 1;
    if (forgot) r->forgotten += 1;
    return forgot;
}

static void retry_obs(const gptps_event *ev, void *ud)
{
    gptps_retry *r = (gptps_retry *)ud;
    const gptps_measure *m, *ms;
    const rule *x;
    size_t i;
    int say = 0;
    switch (ev->kind) {
    case GPTPS_EV_FAILED:
        m = gptps_event_measure(ev, GPTPS_M_EXIT_CODE);
        ms = gptps_event_measure(ev, GPTPS_M_START_ERROR);   /* ABI 2.8: it never started */
        apx_mutex_lock(&r->mu);
        x = ms ? find_rule(r, ev->task_name, 1, 0)
          : (m && m->value <= 0xFFFFFFFFu) ? find_rule(r, ev->task_name, 0, (uint32_t)m->value) : NULL;
        if (x && x->wait_ms) {
            if (put_pend(r, ev->handle, x->wait_ms) && !r->forgot_said) { r->forgot_said = 1; say = 1; }
        } else if ((i = find_pend(r, ev->handle)) != (size_t)-1) {
            drop_pend(r, i);                                  /* this failure has no verdict */
        }
        apx_mutex_unlock(&r->mu);
        if (say)
            gptps_log(NULL, GPTPS_LOG_WARN, "gptps_retry: more failed items are waiting for a retry decision "
                      "than it keeps (4096); the oldest decision was dropped and that item retries as its "
                      "policy says. gptps_retry_forgotten() counts them");
        break;
    case GPTPS_EV_FINISHED:
    case GPTPS_EV_DEAD_LETTERED:
    case GPTPS_EV_DROPPED:
        apx_mutex_lock(&r->mu);
        if ((i = find_pend(r, ev->handle)) != (size_t)-1) drop_pend(r, i);
        apx_mutex_unlock(&r->mu);
        break;
    default:
        break;
    }
}

static gptps_admit_decision retry_fn(const gptps_constraint_input *in, uint32_t *retry_after_ms, void *ud)
{
    gptps_retry *r = (gptps_retry *)ud;
    gptps_admit_decision d = GPTPS_ADMIT;
    uint64_t now;
    size_t i;
    if (!r->npend) return GPTPS_ADMIT;   /* the common case; a stale read only delays a verdict a pass */
    now = in->struct_size >= offsetof(gptps_constraint_input, now_ms) + sizeof in->now_ms
        ? in->now_ms : gptps_now_ms(NULL);
    apx_mutex_lock(&r->mu);
    i = find_pend(r, in->handle);
    if (i != (size_t)-1) {
        verdict *v = &r->pend[i];
        if (v->wait_ms == GPTPS_RETRY_NEVER) {
            drop_pend(r, i);
            r->refused += 1;
            d = GPTPS_DENY;
        } else {
            if (!v->until_ms) { v->until_ms = now + v->wait_ms; r->delayed += 1; }
            if (now < v->until_ms) {
                uint64_t left = v->until_ms - now;
                *retry_after_ms = left > 0xFFFFFFFEu ? 0xFFFFFFFEu : (uint32_t)left;
                d = GPTPS_DEFER;
            } else {
                drop_pend(r, i);           /* its wait is over: admitted as usual */
            }
        }
    }
    apx_mutex_unlock(&r->mu);
    return d;
}

gptps_status gptps_retry_install(gptps *e, gptps_retry **out)
{
    gptps_retry *r;
    gptps_status st;
    if (!e || !out) return GPTPS_E_INVAL;
    *out = NULL;
    r = (gptps_retry *)calloc(1, sizeof *r);
    if (!r) return GPTPS_E_NOMEM;
    apx_mutex_init(&r->mu);
    st = gptps_register_observer(e, retry_obs, r);
    if (st != GPTPS_OK) { gptps_retry_close(r); return st; }
    st = gptps_register_constraint(e, retry_fn, r);
    if (st != GPTPS_OK) {
        gptps_unregister_observer(e, retry_obs, r);
        gptps_retry_close(r);
        return st;
    }
    *out = r;
    return GPTPS_OK;
}

static gptps_status set_rule(gptps_retry *r, const char *task_name, int start, uint32_t exit_code,
                             uint32_t wait_ms)
{
    size_t i;
    if (!r || (task_name && strlen(task_name) > NAME_MAX_LEN)) return GPTPS_E_INVAL;
    apx_mutex_lock(&r->mu);
    for (i = 0; i < r->nrules; ++i) {
        rule *x = &r->rules[i];
        if (x->start == start && (start || x->code == exit_code) &&
            (task_name ? (!x->any && strcmp(x->task, task_name) == 0) : x->any)) {
            x->wait_ms = wait_ms;                         /* replaces */
            apx_mutex_unlock(&r->mu);
            return GPTPS_OK;
        }
    }
    if (r->nrules == r->caprules) {
        size_t ncap = r->caprules ? r->caprules * 2 : 8;
        rule *nr = (rule *)realloc(r->rules, ncap * sizeof *nr);
        if (!nr) { apx_mutex_unlock(&r->mu); return GPTPS_E_NOMEM; }
        r->rules = nr; r->caprules = ncap;
    }
    memset(&r->rules[r->nrules], 0, sizeof *r->rules);
    r->rules[r->nrules].any = task_name == NULL;
    if (task_name) memcpy(r->rules[r->nrules].task, task_name, strlen(task_name) + 1);
    r->rules[r->nrules].start = start;
    r->rules[r->nrules].code = exit_code;
    r->rules[r->nrules].wait_ms = wait_ms;
    r->nrules += 1;
    apx_mutex_unlock(&r->mu);
    return GPTPS_OK;
}

gptps_status gptps_retry_on_exit(gptps_retry *r, const char *task_name, uint32_t exit_code,
                                 uint32_t wait_ms)
{
    return set_rule(r, task_name, 0, exit_code, wait_ms);
}

gptps_status gptps_retry_on_start_error(gptps_retry *r, const char *task_name, uint32_t wait_ms)
{
    return set_rule(r, task_name, 1, 0, wait_ms);
}

uint64_t gptps_retry_forgotten(const gptps_retry *r)
{
    uint64_t n;
    if (!r) return 0;
    apx_mutex_lock((apx_mutex *)&r->mu);
    n = r->forgotten;
    apx_mutex_unlock((apx_mutex *)&r->mu);
    return n;
}

uint64_t gptps_retry_refused(const gptps_retry *r)
{
    uint64_t n;
    if (!r) return 0;
    apx_mutex_lock((apx_mutex *)&r->mu);
    n = r->refused;
    apx_mutex_unlock((apx_mutex *)&r->mu);
    return n;
}

uint64_t gptps_retry_delayed(const gptps_retry *r)
{
    uint64_t n;
    if (!r) return 0;
    apx_mutex_lock((apx_mutex *)&r->mu);
    n = r->delayed;
    apx_mutex_unlock((apx_mutex *)&r->mu);
    return n;
}

void gptps_retry_close(gptps_retry *r)
{
    if (!r) return;
    free(r->rules);
    free(r->pend);
    apx_mutex_destroy(&r->mu);
    free(r);
}
