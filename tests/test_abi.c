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

    /* The two ABI invariants a binary plugin's survival rests on. These are compile-
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
    }

    if (fails) { printf("%d abi check(s) FAILED\n", fails); return 1; }
    printf("all abi (append-safe guard) checks passed\n");
    return 0;
}
