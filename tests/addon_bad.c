/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_bad.c - an add-on with a wrong ABI magic. The loader must reject it
 * with GPTPS_E_ABI (proves the magic/version/size gate works).
 */
#include "gptps.h"

/* Hand-built rather than via GPTPS_ADDON_INIT, so exactly ONE failure axis is wrong:
 * the magic. struct_size and abi_version_major are deliberately CORRECT, which is
 * what proves the loader checks the magic specifically rather than tripping over a
 * size or version mismatch. Every field is initialized explicitly (the ABI 2.1 tail
 * included) so -Wmissing-field-initializers stays clean as the struct grows. */
static const gptps_addon g_bad = {
    sizeof(gptps_addon),
    0xBADBAD00u,                 /* wrong magic - the one thing under test */
    GPTPS_ABI_VERSION_MAJOR,
    "bad",
    GPTPS_SEAM_TASK,
    0,                           /* setup    */
    0,                           /* teardown */
    0,                           /* ns       (ABI 2.1) */
    0                            /* disable  (ABI 2.1) */
};

GPTPS_ADDON_EXPORT const gptps_addon *gptps_addon_init(const gptps_api_routines *api)
{
    (void)api;
    return &g_bad;
}
