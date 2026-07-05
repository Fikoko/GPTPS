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

    if (fails) { printf("%d abi check(s) FAILED\n", fails); return 1; }
    printf("all abi (append-safe guard) checks passed\n");
    return 0;
}
