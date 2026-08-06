/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * config.c - config model + auto-tune (T6).
 *
 * Built before the pool/loader because gptps_open() depends on resolved limits.
 * This slice owns the resolution rule and the auto-tune defaults. The config
 * FILE is parsed by config_toml.c and applied in gptps_open(): [limits] seed the
 * values resolved here, and [task_defaults]/[tasks.*] override per-task policy at
 * registration. ([hardware_patterns.*] matching is a later increment.)
 */
#include "gptps.h"
#include "gptps_hal.h"
#include "gptps_internal.h"

/* 0.75 of detected RAM, expressed as a fraction to stay integer-only. */
#define GPTPS_MEM_FRACTION_NUM 3u
#define GPTPS_MEM_FRACTION_DEN 4u
/* Floor used only when RAM is undetectable, so the budget is never zero. */
#define GPTPS_MEM_FLOOR_BYTES  (64ull * 1024ull * 1024ull)

gptps_status gptps_config_resolve(const gptps_limits *in, gptps_limits *out)
{
    gptps_hwinfo hw;
    gptps_status s;
    uint32_t conc;
    uint64_t mem;

    if (!out) return GPTPS_E_INVAL;

    s = gptps_hal_detect(&hw);
    if (s != GPTPS_OK) return s;

    /* concurrency: explicit wins; else detected cores; never below 1. */
    conc = (in && in->max_concurrent_tasks) ? in->max_concurrent_tasks : hw.cpu_count;
    if (conc == 0) conc = 1u;

    /* memory: explicit wins; else 0.75 * RAM; else floor. */
    if (in && in->max_memory_bytes) {
        mem = in->max_memory_bytes;
    } else if (hw.ram_bytes) {
        mem = hw.ram_bytes / GPTPS_MEM_FRACTION_DEN * GPTPS_MEM_FRACTION_NUM;
        if (mem == 0) mem = GPTPS_MEM_FLOOR_BYTES;
    } else {
        mem = GPTPS_MEM_FLOOR_BYTES;
    }

    out->struct_size = sizeof *out;
    out->max_concurrent_tasks = conc;
    out->max_memory_bytes = mem;
    out->max_intake_depth = in ? in->max_intake_depth : 0; /* 0 => unbounded intake */
    return GPTPS_OK;
}
