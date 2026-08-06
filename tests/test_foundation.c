/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_foundation.c - tests for the HAL (T3) + config auto-tune (T6).
 * Zero-dependency: a CHECK macro + nonzero exit on failure, run by CTest.
 * (Unity wrapping is a reporting nicety to layer on later; the assertions are
 * the substance.)
 */
#include "gptps.h"
#include "gptps_hal.h"
#include "gptps_internal.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main(void)
{
    /* --- hardware detection --- */
    gptps_hwinfo hw;
    CHECK(gptps_hal_detect(&hw) == GPTPS_OK);
    CHECK(hw.cpu_count >= 1);
    CHECK(hw.ram_bytes > 0);
    CHECK(gptps_hal_detect(NULL) == GPTPS_E_INVAL);

    /* --- monotonic clock is non-decreasing --- */
    {
        uint64_t a = gptps_hal_monotonic_ms();
        uint64_t b = gptps_hal_monotonic_ms();
        CHECK(b >= a);
    }

    /* --- cancel flag set/get across the accessor --- */
    {
        gptps_flag *f = gptps_flag_create(false);
        CHECK(f != NULL);
        CHECK(gptps_flag_get(f) == false);
        gptps_flag_set(f, true);
        CHECK(gptps_flag_get(f) == true);
        gptps_flag_set(f, false);
        CHECK(gptps_flag_get(f) == false);
        gptps_flag_destroy(f);
    }

    /* --- config auto-tune: zeros resolve from hardware --- */
    {
        /* memset + explicit struct_size rather than a positional initializer list:
         * the list stops naming fields as soon as gptps_limits grows, which is a
         * -Wmissing-field-initializers error under -Werror and, worse, leaves any
         * appended field indeterminate in a test about DEFAULTS. */
        gptps_limits zero, r;
        memset(&zero, 0, sizeof zero);
        zero.struct_size = sizeof zero;
        CHECK(gptps_config_resolve(&zero, &r) == GPTPS_OK);
        CHECK(r.max_concurrent_tasks == hw.cpu_count);
        CHECK(r.max_memory_bytes > 0);
        CHECK(r.max_memory_bytes <= hw.ram_bytes); /* 0.75 of RAM never exceeds RAM */
    }

    /* --- explicit values win over auto-tune --- */
    {
        gptps_limits ex, r;
        memset(&ex, 0, sizeof ex);
        ex.struct_size = sizeof ex;
        ex.max_concurrent_tasks = 3;
        ex.max_memory_bytes = 123456789ull;
        CHECK(gptps_config_resolve(&ex, &r) == GPTPS_OK);
        CHECK(r.max_concurrent_tasks == 3u);
        CHECK(r.max_memory_bytes == 123456789ull);
    }

    /* --- NULL handling --- */
    {
        gptps_limits r;
        CHECK(gptps_config_resolve(NULL, &r) == GPTPS_OK); /* "all auto" */
        CHECK(r.max_concurrent_tasks >= 1);
        CHECK(gptps_config_resolve(&r, NULL) == GPTPS_E_INVAL);
    }

    if (fails) { printf("%d check(s) FAILED\n", fails); return 1; }
    printf("all foundation checks passed\n");
    return 0;
}
