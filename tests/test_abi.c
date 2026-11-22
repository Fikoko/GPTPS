/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_abi.c - append-safe struct guards. A caller-supplied INPUT struct is
 * validated against a FROZEN minimum (the end of the last current field), not the
 * live sizeof - so a caller compiled against today's header keeps validating after
 * the core appends a field. This checks the floor directly: struct_size == the
 * frozen minimum is accepted; one byte below it is rejected.
 */
#include "gptps.h"
#include "gptps_internal.h"   /* GPTPS_CAP_*: the executors' bit set, checked like the public ones */
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* the frozen floors, recomputed here the same way the core defines them */
#define CONFIG_FLOOR  (offsetof(gptps_config, mode) + sizeof(((gptps_config *)0)->mode))
#define OPTS_FLOOR    (offsetof(gptps_submit_options, timeout_ms) + sizeof(((gptps_submit_options *)0)->timeout_ms))
#define ALLOC_FLOOR   (offsetof(gptps_allocator, user_data) + sizeof(((gptps_allocator *)0)->user_data))
/* The add-on descriptor's floor: the end of `teardown`, its last v1.0 field. Unlike
 * the three above this one is checked by the LOADER, and it is the floor that must
 * hold hardest - every binary plugin ever built declares sizeof(gptps_addon) as it
 * knew it, and a floor that drifted upward would lock out every existing .so at
 * once. Appending to gptps_addon must therefore never move this. */
#define ADDON_FLOOR   (offsetof(gptps_addon, teardown) + sizeof(((gptps_addon *)0)->teardown))

/* ---- flag families: every bit once ---------------------------------------
 * A flag word is only as good as its bits being distinct. GPTPS_TASK_DIE_WITH_HOST
 * shipped (in 2.8, unreleased) on the same bit as GPTPS_TASK_RETIRE_ON_OK and nothing
 * noticed, because no test compared flag values with each other. Two checks close it:
 *
 *   1. Each family listed below is a set of single, distinct, non-zero bits.
 *   2. The list is the WHOLE family: the public header is read back and every
 *      `#define <PREFIX><NAME> <value>` in it must be one listed here, with the same
 *      value. So a new flag cannot be added to the header without being added here -
 *      and then checked against its siblings - nor pick a bit by copy and paste.
 *
 * Values are only ever named through their macros here, so the check holds whatever
 * bit a flag moves to. */
typedef struct { const char *name; unsigned long long value; } flag_def;
#define FLAG(m) { #m, (unsigned long long)(m) }

static int single_bit(unsigned long long v) { return v != 0 && (v & (v - 1)) == 0; }

static void check_bits(const char *family, const flag_def *f, size_t n)
{
    size_t i, j;
    for (i = 0; i < n; ++i) {
        if (!single_bit(f[i].value)) {
            printf("FAIL %s: %s = 0x%llx is not a single bit\n", family, f[i].name, f[i].value);
            ++fails;
        }
        for (j = i + 1; j < n; ++j)
            if (f[i].value == f[j].value) {
                printf("FAIL %s: %s and %s are the same bit (0x%llx)\n",
                       family, f[i].name, f[j].name, f[i].value);
                ++fails;
            }
    }
}

/* Every `#define <prefix>X <number>` in the header, less `skip` (macros that share the
 * prefix but are not flags), must be in `f` with the same value. */
static void check_family_complete(const char *hdr, const char *prefix, const char *const *skip,
                                  const flag_def *f, size_t n)
{
    FILE *fp = fopen(hdr, "r");
    char line[512];
    size_t plen = strlen(prefix), found = 0;
    if (!fp) { printf("FAIL cannot open %s to read the %s flags\n", hdr, prefix); ++fails; return; }
    while (fgets(line, sizeof line, fp)) {
        const char *p = line, *name, *val;
        size_t nlen, i;
        int listed = 0, skipped = 0;
        char *end;
        unsigned long long v;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '#') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "define", 6) != 0) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, prefix, plen) != 0) continue;
        name = p;
        while (isalnum((unsigned char)*p) || *p == '_') ++p;
        nlen = (size_t)(p - name);
        for (i = 0; skip && skip[i]; ++i)
            if (strlen(skip[i]) == nlen && strncmp(skip[i], name, nlen) == 0) skipped = 1;
        if (skipped) continue;
        while (*p == ' ' || *p == '\t') ++p;
        val = p;
        v = strtoull(val, &end, 0);
        if (end == val) {
            printf("FAIL %.*s: value is not a plain number; list it in the skips or write it as one\n",
                   (int)nlen, name);
            ++fails;
            continue;
        }
        ++found;
        for (i = 0; i < n; ++i)
            if (strlen(f[i].name) == nlen && strncmp(f[i].name, name, nlen) == 0) {
                listed = 1;
                if (f[i].value != v) {
                    printf("FAIL %.*s: header says 0x%llx, compiled 0x%llx\n",
                           (int)nlen, name, v, f[i].value);
                    ++fails;
                }
            }
        if (!listed) {
            printf("FAIL %.*s is a new %s flag: add it to test_abi.c's list so its bit is "
                   "checked against the others\n", (int)nlen, name, prefix);
            ++fails;
        }
    }
    fclose(fp);
    if (found != n) {
        printf("FAIL %s: %lu listed, %lu in the header\n", prefix,
               (unsigned long)n, (unsigned long)found);
        ++fails;
    }
}

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static void *a_malloc(size_t n, void *u)  { (void)u; return malloc(n); }
static void *a_realloc(void *p, size_t n, void *u) { (void)u; return realloc(p, n); }
static void  a_free(void *p, void *u)     { (void)u; free(p); }

int main(void)
{
    /* config: accepted at the floor, rejected one byte below */
    {
        gptps *e = NULL;
        gptps_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.limits.struct_size = sizeof cfg.limits;
        cfg.struct_size = CONFIG_FLOOR;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);     /* minimal (older-style) caller accepted */
        if (e) { gptps_shutdown(e); e = NULL; }
        cfg.struct_size = CONFIG_FLOOR - 1;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_E_INVAL); /* below the floor rejected */
        CHECK(e == NULL);
    }

    /* submit_options: accepted at the floor, rejected below */
    {
        gptps *e = NULL;
        gptps_config cfg;
        gptps_task_def d;
        gptps_submit_options opts;
        memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK); if (!e) return 1;
        memset(&d, 0, sizeof d); d.struct_size = sizeof d; d.name = "n"; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost; d.default_policy.struct_size = sizeof d.default_policy;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);

        memset(&opts, 0, sizeof opts);
        opts.struct_size = OPTS_FLOOR;
        CHECK(gptps_submit_ex(e, "n", NULL, 0, &opts, NULL) == GPTPS_OK);
        opts.struct_size = OPTS_FLOOR - 1;
        CHECK(gptps_submit_ex(e, "n", NULL, 0, &opts, NULL) == GPTPS_E_INVAL);
        gptps_shutdown(e);
    }

    /* allocator: accepted at the floor, rejected below (then reset to libc) */
    {
        gptps_allocator a;
        memset(&a, 0, sizeof a);
        a.malloc_fn = a_malloc; a.realloc_fn = a_realloc; a.free_fn = a_free;
        a.struct_size = ALLOC_FLOOR;
        CHECK(gptps_set_allocator(&a) == GPTPS_OK);
        a.struct_size = ALLOC_FLOOR - 1;
        CHECK(gptps_set_allocator(&a) == GPTPS_E_INVAL);
        CHECK(gptps_set_allocator(NULL) == GPTPS_OK);   /* reset process-wide allocator */
    }

    /* The ABI invariants a binary plugin's survival rests on. The first two are compile-
     * time, because a runtime check would come too late: the failure they guard
     * against is "the header changed and every already-built .so stopped loading",
     * and by the time anyone runs a test the .so in question is somebody else's. */
    {
        /* 1) The add-on descriptor still starts with struct_size, and its floor still
         *    lies WITHIN the struct. sizeof(gptps_addon) may grow as fields are
         *    appended - that is expected - but `teardown` must remain the last field
         *    of the v1.0 set, so the floor never climbs past it. Expressed with
         *    offsets rather than a byte count, because the count differs on i386 and
         *    s390x, both of which CI builds. */
        typedef char addon_struct_size_is_first[(offsetof(gptps_addon, struct_size) == 0) ? 1 : -1];
        typedef char addon_floor_within_struct[(ADDON_FLOOR <= sizeof(gptps_addon)) ? 1 : -1];
        /* 2) The host table only ever GREW. A plugin guards each routine with
         *    `api->struct_size > offsetof(..., routine)`, so a routine that moved
         *    would silently mean something else. Pin the v1.0 head: register_task
         *    must remain the first routine, immediately after the three scalars. */
        typedef char table_head_is_stable[
            (offsetof(gptps_api_routines, register_task) ==
             offsetof(gptps_api_routines, abi_version_minor) + sizeof(uint32_t)) ? 1 : -1];
        (void)sizeof(addon_struct_size_is_first);
        (void)sizeof(addon_floor_within_struct);
        (void)sizeof(table_head_is_stable);
        /* And the 2.1 additions really are additions, not a reshuffle. */
        CHECK(sizeof(gptps_api_routines) > offsetof(gptps_api_routines, is_cancelled));
        CHECK(offsetof(gptps_api_routines, set_scheduler) < offsetof(gptps_api_routines, is_cancelled));
        /* 3) gptps_event: struct_size first, and 2.3's `flags` APPENDED after the 2.2
         *    tail (result_len), so an observer's `struct_size` guard can tell an older
         *    engine's shorter struct apart. */
        CHECK(offsetof(gptps_event, struct_size) == 0);
        CHECK(offsetof(gptps_event, flags) >= offsetof(gptps_event, result_len) + sizeof(size_t));
        CHECK(sizeof(gptps_event) >= offsetof(gptps_event, flags) + sizeof(uint32_t));
    }

    /* 4) What 2.4 appends to gptps_config GROWS it, on every ABI: each new field
     *    starts at or past the sizeof of the struct before it, never in that struct's
     *    tail padding, where struct_size could not tell an older caller's leftover
     *    bytes from a value. The older layouts, rebuilt from the same members: */
    {
        typedef struct { size_t struct_size; const char *config_path; gptps_limits limits;
                         gptps_run_mode mode; } config_1_5;
        typedef struct { size_t struct_size; const char *config_path; gptps_limits limits;
                         gptps_run_mode mode; uint64_t max_items; uint32_t max_payload_bytes;
                         uint32_t max_result_bytes; } config_bounded;
        typedef char max_items_grows[
            (offsetof(gptps_config, max_items) >= sizeof(config_1_5)) ? 1 : -1];
        typedef char dead_letters_grows[
            (offsetof(gptps_config, max_dead_letters) >= sizeof(config_bounded)) ? 1 : -1];
        typedef char grace_inside[
            (offsetof(gptps_config, shutdown_grace_ms) + sizeof(uint32_t) <= sizeof(gptps_config)) ? 1 : -1];
        (void)sizeof(max_items_grows);
        (void)sizeof(dead_letters_grows);
        (void)sizeof(grace_inside);
        CHECK(GPTPS_LIMIT_NONE == 0xFFFFFFFFu);
    }

    /* 5) What 2.5 appends to gptps_event starts past the end of a 2.4 event on every
     *    ABI. Where uint64_t aligns to 8 on a 32-bit ABI (ARMv7, Win32) the 2.4 struct
     *    ended in 4 bytes of tail padding after `flags`; `reserved` fills them, so
     *    `measures` cannot land there, where a 2.4 struct_size would cover it. The 2.4
     *    layout, rebuilt from the same members: */
    {
        typedef struct { size_t struct_size; gptps_event_kind kind; gptps_handle handle;
                         const char *task_name; uint64_t ts_ms; gptps_status status;
                         uint32_t attempt; uint64_t mem_bytes; const void *result;
                         size_t result_len; uint32_t flags; } event_2_4;
        typedef char measures_grow[
            (offsetof(gptps_event, measures) >= sizeof(event_2_4)) ? 1 : -1];
        typedef char flags_unmoved[
            (offsetof(gptps_event, flags) == offsetof(event_2_4, flags)) ? 1 : -1];
        typedef char count_inside[
            (offsetof(gptps_event, n_measures) + sizeof(size_t) <= sizeof(gptps_event)) ? 1 : -1];
        /* gptps_measure is an array element, so it is frozen: these are its fields, in
         * this order, for good. */
        typedef char measure_order[
            (offsetof(gptps_measure, name) == 0 &&
             offsetof(gptps_measure, value) < offsetof(gptps_measure, unit) &&
             offsetof(gptps_measure, unit) + sizeof(uint16_t) == offsetof(gptps_measure, kind) &&
             offsetof(gptps_measure, kind) + sizeof(uint16_t) == offsetof(gptps_measure, flags) &&
             offsetof(gptps_measure, flags) + sizeof(uint32_t) <= offsetof(gptps_measure, method)) ? 1 : -1];
        (void)sizeof(measures_grow);
        (void)sizeof(flags_unmoved);
        (void)sizeof(count_inside);
        (void)sizeof(measure_order);
        CHECK(GPTPS_EV_SAMPLE == GPTPS_EV_DROPPED + 1);
        CHECK(GPTPS_UNIT_BYTES == 1 && GPTPS_UNIT_FLAG == 4 && GPTPS_UNIT_CODE == 5);
        CHECK(GPTPS_MEASURE_PEAK == 1 && GPTPS_MEASURE_CURRENT == 4);
    }

    /* 6) What 2.6, 2.7 and 2.8 append. Each block of new fields starts at or past the
     *    sizeof of the struct as it was before (never in its tail padding, where an
     *    older caller's struct_size would cover it), the new fields keep their order,
     *    and the last one lies inside the struct. The older layouts, rebuilt from the
     *    same members: */
    {
        /* gptps_task_def: 2.6 the caps after `flags`; 2.7 work_dir, env,
         * stop_grace_ms, stderr_tail_bytes. */
        typedef struct { size_t struct_size; const char *name; gptps_run_fn run;
                         gptps_cost_fn cost; void *user_data; gptps_exec_kind exec;
                         gptps_cost default_cost; gptps_failure_policy default_policy;
                         const char *const *argv; void (*child_setup)(void *);
                         uint64_t flags; } task_def_2_5;
        typedef struct { task_def_2_5 base; uint64_t cpu_max_millicpus;
                         uint64_t io_max_read_bps; uint64_t io_max_write_bps; } task_def_2_6;
        typedef char td_flags_unmoved[
            (offsetof(gptps_task_def, flags) == offsetof(task_def_2_5, flags)) ? 1 : -1];
        typedef char td_caps_grow[
            (offsetof(gptps_task_def, cpu_max_millicpus) >= sizeof(task_def_2_5)) ? 1 : -1];
        typedef char td_caps_order[
            (offsetof(gptps_task_def, io_max_read_bps) == offsetof(gptps_task_def, cpu_max_millicpus) + 8 &&
             offsetof(gptps_task_def, io_max_write_bps) == offsetof(gptps_task_def, io_max_read_bps) + 8) ? 1 : -1];
        typedef char td_2_7_grows[
            (offsetof(gptps_task_def, work_dir) >= sizeof(task_def_2_6)) ? 1 : -1];
        typedef char td_2_7_order[
            (offsetof(gptps_task_def, env) > offsetof(gptps_task_def, work_dir) &&
             offsetof(gptps_task_def, stop_grace_ms) >= offsetof(gptps_task_def, env) + sizeof(void *) &&
             offsetof(gptps_task_def, stderr_tail_bytes) == offsetof(gptps_task_def, stop_grace_ms) + sizeof(uint32_t)) ? 1 : -1];
        typedef char td_tail_inside[
            (offsetof(gptps_task_def, stderr_tail_bytes) + sizeof(uint32_t) <= sizeof(gptps_task_def)) ? 1 : -1];

        /* gptps_submit_options: 2.6 deadline_in_ms. */
        typedef struct { size_t struct_size; unsigned flags; int32_t priority;
                         gptps_failure_policy policy; uint32_t timeout_ms; } opts_2_5;
        typedef char opts_deadline_grows[
            (offsetof(gptps_submit_options, deadline_in_ms) >= sizeof(opts_2_5)) ? 1 : -1];
        typedef char opts_deadline_inside[
            (offsetof(gptps_submit_options, deadline_in_ms) + sizeof(uint64_t) <= sizeof(gptps_submit_options)) ? 1 : -1];

        /* gptps_constraint_input: 2.6 deadline_ms, now_ms. */
        typedef struct { size_t struct_size; const char *task_name; const gptps_cost *cost;
                         gptps_handle handle; const void *payload; size_t payload_len; } cin_2_5;
        typedef char cin_deadline_grows[
            (offsetof(gptps_constraint_input, deadline_ms) >= sizeof(cin_2_5)) ? 1 : -1];
        typedef char cin_now_after[
            (offsetof(gptps_constraint_input, now_ms) == offsetof(gptps_constraint_input, deadline_ms) + 8 &&
             offsetof(gptps_constraint_input, now_ms) + 8 <= sizeof(gptps_constraint_input)) ? 1 : -1];

        /* gptps_sched_input: 2.6 deadline_ms. */
        typedef struct { size_t struct_size; const char *task_name; const gptps_cost *cost;
                         gptps_handle handle; int32_t priority; uint32_t attempt;
                         uint64_t enqueue_ms; const void *payload; size_t payload_len; } sin_2_5;
        typedef char sin_deadline_grows[
            (offsetof(gptps_sched_input, deadline_ms) >= sizeof(sin_2_5)) ? 1 : -1];
        typedef char sin_deadline_inside[
            (offsetof(gptps_sched_input, deadline_ms) + 8 <= sizeof(gptps_sched_input)) ? 1 : -1];

        /* gptps_event: 2.7 stderr_tail / stderr_tail_len after 2.5's measures. */
        typedef struct { size_t struct_size; gptps_event_kind kind; gptps_handle handle;
                         const char *task_name; uint64_t ts_ms; gptps_status status;
                         uint32_t attempt; uint64_t mem_bytes; const void *result;
                         size_t result_len; uint32_t flags; uint32_t reserved;
                         const gptps_measure *measures; size_t n_measures; } event_2_6;
        typedef char ev_tail_grows[
            (offsetof(gptps_event, stderr_tail) >= sizeof(event_2_6)) ? 1 : -1];
        typedef char ev_tail_inside[
            (offsetof(gptps_event, stderr_tail_len) == offsetof(gptps_event, stderr_tail) + sizeof(void *) &&
             offsetof(gptps_event, stderr_tail_len) + sizeof(size_t) <= sizeof(gptps_event)) ? 1 : -1];

        /* gptps_dead_letter: 2.6 options; 2.7 stderr_tail / _len; 2.8 measures /
         * n_measures. */
        typedef struct { size_t struct_size; gptps_handle handle; const char *task_name;
                         gptps_status status; uint32_t attempts; const void *payload;
                         size_t payload_len; } dl_2_5;
        typedef struct { dl_2_5 base; gptps_submit_options options; } dl_2_6;
        typedef struct { dl_2_6 base; const char *stderr_tail; size_t stderr_tail_len; } dl_2_7;
        typedef char dl_options_grows[
            (offsetof(gptps_dead_letter, options) >= sizeof(dl_2_5)) ? 1 : -1];
        typedef char dl_tail_grows[
            (offsetof(gptps_dead_letter, stderr_tail) >= sizeof(dl_2_6)) ? 1 : -1];
        typedef char dl_measures_grow[
            (offsetof(gptps_dead_letter, measures) >= sizeof(dl_2_7)) ? 1 : -1];
        typedef char dl_count_inside[
            (offsetof(gptps_dead_letter, n_measures) == offsetof(gptps_dead_letter, measures) + sizeof(void *) &&
             offsetof(gptps_dead_letter, n_measures) + sizeof(size_t) <= sizeof(gptps_dead_letter)) ? 1 : -1];

        (void)sizeof(td_flags_unmoved); (void)sizeof(td_caps_grow); (void)sizeof(td_caps_order);
        (void)sizeof(td_2_7_grows); (void)sizeof(td_2_7_order); (void)sizeof(td_tail_inside);
        (void)sizeof(opts_deadline_grows); (void)sizeof(opts_deadline_inside);
        (void)sizeof(cin_deadline_grows); (void)sizeof(cin_now_after);
        (void)sizeof(sin_deadline_grows); (void)sizeof(sin_deadline_inside);
        (void)sizeof(ev_tail_grows); (void)sizeof(ev_tail_inside);
        (void)sizeof(dl_options_grows); (void)sizeof(dl_tail_grows);
        (void)sizeof(dl_measures_grow); (void)sizeof(dl_count_inside);
        /* 2.9: gptps_engine_totals, new - struct_size first, then four uint64_t in
         * order, its floor (GPTPS_ENGINE_TOTALS_MIN_SIZE) the end of `dead`. */
        {
            typedef char et_size_first[(offsetof(gptps_engine_totals, struct_size) == 0) ? 1 : -1];
            typedef char et_order[
                (offsetof(gptps_engine_totals, queued) >= sizeof(size_t) &&
                 offsetof(gptps_engine_totals, delayed) == offsetof(gptps_engine_totals, queued) + 8 &&
                 offsetof(gptps_engine_totals, running) == offsetof(gptps_engine_totals, delayed) + 8 &&
                 offsetof(gptps_engine_totals, dead) == offsetof(gptps_engine_totals, running) + 8 &&
                 offsetof(gptps_engine_totals, dead) + 8 <= sizeof(gptps_engine_totals)) ? 1 : -1];
            typedef char et_wide[(sizeof(((gptps_engine_totals *)0)->queued) == 8) ? 1 : -1];
            gptps_status (*fn)(gptps *, gptps_engine_totals *) = gptps_engine_counts;
            (void)sizeof(et_size_first); (void)sizeof(et_order); (void)sizeof(et_wide);
            CHECK(fn != NULL);
        }
        /* 2.10: gptps_settings_provisional and gptps_settings_hold, new; the host table
         * appends settings_provisional, the last member, right after event_measure. */
        {
            typedef char hp_after[(offsetof(gptps_api_routines, settings_provisional) ==
                                   offsetof(gptps_api_routines, event_measure) + sizeof(void (*)(void))) ? 1 : -1];
            typedef char hp_last[(offsetof(gptps_api_routines, settings_provisional) + sizeof(void (*)(void)) ==
                                  sizeof(gptps_api_routines)) ? 1 : -1];
            int (*fp)(gptps *, const char *) = gptps_settings_provisional;
            gptps_status (*fh)(gptps *, int) = gptps_settings_hold;
            gptps_api_routines api;
            (void)sizeof(hp_after); (void)sizeof(hp_last);
            api.settings_provisional = gptps_settings_provisional;
            CHECK(fp != NULL && fh != NULL && api.settings_provisional == fp);
        }
        /* The ABI this file guards. A new minor appends something: add its guards
         * above, then move this. */
        CHECK(GPTPS_ABI_VERSION_MAJOR == 2u && GPTPS_ABI_VERSION_MINOR == 10u);
    }

    /* 7) Every flag family: single, distinct bits, and the whole family listed. */
    {
        static const flag_def task[] = {
            FLAG(GPTPS_TASK_SERVICE), FLAG(GPTPS_TASK_DIE_WITH_HOST), FLAG(GPTPS_TASK_RETIRE_ON_OK)
        };
        static const char *const task_skip[] = { "GPTPS_TASK_NAME_MAX", NULL };
        static const flag_def submit[] = {
            FLAG(GPTPS_SUBMIT_PRIORITY), FLAG(GPTPS_SUBMIT_POLICY),
            FLAG(GPTPS_SUBMIT_TIMEOUT_MS), FLAG(GPTPS_SUBMIT_DEADLINE)
        };
        static const flag_def evflag[] = {
            FLAG(GPTPS_EV_FLAG_SHUTDOWN), FLAG(GPTPS_EV_FLAG_SELF_CANCELLED)
        };
        static const flag_def sched[] = { FLAG(GPTPS_SCHED_REPLACE), FLAG(GPTPS_SCHED_STABLE) };
        /* Internal, but the same word-of-bits shape: what an executor reports applied. */
        static const flag_def cap[] = {
            FLAG(GPTPS_CAP_CPU), FLAG(GPTPS_CAP_IO), FLAG(GPTPS_CAP_MEM_CG), FLAG(GPTPS_CAP_GRACE),
            FLAG(GPTPS_CAP_DIE), FLAG(GPTPS_CAP_IO_ONE), FLAG(GPTPS_CAP_START)
        };
        check_bits("GPTPS_TASK_*", task, NELEM(task));
        check_bits("GPTPS_SUBMIT_*", submit, NELEM(submit));
        check_bits("GPTPS_EV_FLAG_*", evflag, NELEM(evflag));
        check_bits("GPTPS_SCHED_*", sched, NELEM(sched));
        check_bits("GPTPS_CAP_*", cap, NELEM(cap));
        /* The task flags live in a uint64_t; the others in 32-bit words. */
        CHECK((GPTPS_SUBMIT_PRIORITY | GPTPS_SUBMIT_POLICY | GPTPS_SUBMIT_TIMEOUT_MS |
               GPTPS_SUBMIT_DEADLINE) <= 0xFFFFFFFFu);
#if defined(GPTPS_PUBLIC_HEADER)
        check_family_complete(GPTPS_PUBLIC_HEADER, "GPTPS_TASK_", task_skip, task, NELEM(task));
        check_family_complete(GPTPS_PUBLIC_HEADER, "GPTPS_SUBMIT_", NULL, submit, NELEM(submit));
        check_family_complete(GPTPS_PUBLIC_HEADER, "GPTPS_EV_FLAG_", NULL, evflag, NELEM(evflag));
        check_family_complete(GPTPS_PUBLIC_HEADER, "GPTPS_SCHED_", NULL, sched, NELEM(sched));
#else
        (void)task_skip;
        printf("FAIL built without GPTPS_PUBLIC_HEADER: cannot check the flag lists are complete\n");
        ++fails;
#endif
    }

    if (fails) { printf("%d abi check(s) FAILED\n", fails); return 1; }
    printf("all abi (append-safe guard) checks passed\n");
    return 0;
}
