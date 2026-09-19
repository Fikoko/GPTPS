/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/* Release-version contract: numeric macros, string macro, and runtime agree. */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

#define STR_INNER(x) #x
#define STR(x) STR_INNER(x)

int main(void)
{
    static const char numeric[] =
        STR(GPTPS_VERSION_MAJOR) "."
        STR(GPTPS_VERSION_MINOR) "."
        STR(GPTPS_VERSION_PATCH);

    if (strcmp(numeric, GPTPS_VERSION_STRING) != 0) {
        fprintf(stderr, "numeric version %s != string version %s\n",
                numeric, GPTPS_VERSION_STRING);
        return 1;
    }
    if (strcmp(gptps_version(), GPTPS_VERSION_STRING) != 0) {
        fprintf(stderr, "runtime version %s != header version %s\n",
                gptps_version(), GPTPS_VERSION_STRING);
        return 1;
    }
    return 0;
}
