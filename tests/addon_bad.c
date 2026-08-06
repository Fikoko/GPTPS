/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_bad.c - an add-on with a wrong ABI magic. The loader must reject it
 * with GPTPS_E_ABI (proves the magic/version/size gate works).
 */
#include "gptps.h"

static const gptps_addon g_bad = {
    sizeof(gptps_addon),
    0xBADBAD00u,                 /* wrong magic */
    GPTPS_ABI_VERSION_MAJOR,
    "bad",
    GPTPS_SEAM_TASK,
    0, 0
};

GPTPS_ADDON_EXPORT const gptps_addon *gptps_addon_init(const gptps_api_routines *api)
{
    (void)api;
    return &g_bad;
}
