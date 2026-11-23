/* SPDX-License-Identifier: MIT */
/* Copyright (c) nightops00dev */
/* Adapted from nightops00dev's night-window harnesses,
 * https://github.com/nightops00dev/gptps-night-window-harness, MIT; see Fikoko/GPTPS#12 */
/*
 * erp_stop_job.c - long-running vendor job that honours Ctrl+Break (Windows only).
 * Used by pilot_pass3_grace_win.c to measure stop_grace_ms (GPTPS 1.10+).
 * Prints WORK ticks to stderr; on Ctrl+Break rolls back and exits 0.
 */
#include <stdio.h>
#include <windows.h>

static volatile LONG g_break = 0;

static BOOL WINAPI on_ctrl(DWORD kind)
{
    if (kind == CTRL_BREAK_EVENT || kind == CTRL_C_EVENT) {
        InterlockedExchange(&g_break, 1);
        return TRUE;
    }
    return FALSE;
}

int main(void)
{
    int i;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    for (i = 0; i < 600; ++i) {          /* 60 s if never stopped */
        fprintf(stderr, "WORK tick %d\n", i);
        fflush(stderr);
        if (g_break) {
            /* the rollback the grace period exists for */
            Sleep(80);                    /* undo the half-made night posting */
            fprintf(stderr, "ROLLBACK-EVIDENCE tick=%d undone\n", i);
            fflush(stderr);
            return 0;
        }
        Sleep(100);
    }
    return 0;
}
