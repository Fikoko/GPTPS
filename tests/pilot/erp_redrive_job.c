/* SPDX-License-Identifier: MIT */
/* Copyright (c) nightops00dev */
/* Adapted from nightops00dev's night-window harnesses,
 * https://github.com/nightops00dev/gptps-night-window-harness, MIT; see Fikoko/GPTPS#12 */
/*
 * erp_redrive_job.c - vendor job whose licence can be "renewed" overnight (pilot
 * passes three and four).
 *
 * Exit 3 while licence.renewed is absent from its working directory (the task
 * type's work_dir: dead-letter), exit 0 once the morning operator drops that file
 * there (re-drive succeeds). Evidence line on stderr either way, naming the job
 * tag read from the first line of stdin (the payload; "-" when there is none).
 */
#include <stdio.h>
#include <string.h>

int main(void)
{
    char tag[128] = "-";
    char line[128];
    FILE *f;

    if (fgets(line, sizeof line, stdin)) {
        size_t n = strcspn(line, "\r\n");
        line[n] = '\0';
        if (n) snprintf(tag, sizeof tag, "%s", line);
    }
    f = fopen("licence.renewed", "rb");
    if (f) {
        fclose(f);
        fprintf(stderr, "TAIL-EVIDENCE exit=0 marker=renewed job=%s\n", tag);
        return 0;
    }
    fprintf(stderr, "TAIL-EVIDENCE exit=3 marker=licence-missing job=%s\n", tag);
    return 3;
}
