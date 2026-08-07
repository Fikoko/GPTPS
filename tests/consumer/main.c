/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * consumer/main.c - the smallest honest proof that the INSTALL tree works.
 *
 * Includes are unqualified ("gptps_pool.h"), exactly as a consumer writes them, so
 * this only compiles if the add-on targets export their include directory through
 * INSTALL_INTERFACE correctly. It links two add-ons and the core, so it only links
 * if all three landed in the export set in a usable order.
 */
#include "gptps.h"
#include "gptps_pool.h"
#include "gptps_durable_queue.h"
#include <stdio.h>
#include <string.h>

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

int main(void)
{
    gptps_pool *p;
    gptps_task_def d;
    gptps_pool_handle h = 0;

    p = gptps_pool_open(2, NULL);
    if (!p) { printf("pool_open failed\n"); return 1; }

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "t"; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    if (gptps_pool_register_task(p, &d) != GPTPS_OK) { printf("register failed\n"); return 1; }
    if (gptps_pool_submit(p, "t", NULL, 0, &h) != GPTPS_OK) { printf("submit failed\n"); return 1; }
    gptps_pool_close(p);

    printf("consumer OK: gptps %s, core + durable_queue + pool from the install tree\n",
           gptps_version());
    return 0;
}
