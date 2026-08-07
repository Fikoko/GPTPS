/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * hal_win.c - GPTPS Hardware Abstraction Layer, Win32 backend.
 *
 * The POSIX counterpart is hal_posix.c; this implements the same gptps_hal.h
 * interface with Win32 primitives so the C99 core runs unchanged on Windows:
 *   threads      _beginthreadex
 *   mutex        CRITICAL_SECTION
 *   condvar      CONDITION_VARIABLE (+ SleepConditionVariableCS for timed waits)
 *   cancel flag  Interlocked* on a LONG (atomics stay confined to the HAL)
 *   clock        GetTickCount64 (monotonic milliseconds)
 *   hwdetect     GetSystemInfo + GlobalMemoryStatusEx
 *   dynload      LoadLibrary / GetProcAddress / FreeLibrary
 *
 * Every handle is opaque + heap-allocated, so the core never embeds a Win32 type.
 */
#if defined(_WIN32)

#include "gptps.h"
#include "gptps_hal.h"

#include <windows.h>
#include <process.h>   /* _beginthreadex */
#include <stdlib.h>

/* --- hardware detection -------------------------------------------------- */
gptps_status gptps_hal_detect(gptps_hwinfo *out)
{
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    if (!out) return GPTPS_E_INVAL;
    GetSystemInfo(&si);
    out->cpu_count = si.dwNumberOfProcessors ? (unsigned)si.dwNumberOfProcessors : 1u;
    ms.dwLength = sizeof ms;
    out->ram_bytes = GlobalMemoryStatusEx(&ms) ? (uint64_t)ms.ullTotalPhys : 0;
    out->has_gpu = false; /* best-effort: a GPU add-on is the real detector */
    return GPTPS_OK;
}

/* --- monotonic clock (ms) ------------------------------------------------ */
uint64_t gptps_hal_monotonic_ms(void)
{
    return (uint64_t)GetTickCount64(); /* ms since boot; monotonic, never wraps in practice */
}

/* --- cancel flag (atomic LONG) ------------------------------------------- */
struct gptps_flag { volatile LONG v; };

gptps_flag *gptps_flag_create(bool initial)
{
    gptps_flag *f = (gptps_flag *)malloc(sizeof *f);
    if (f) f->v = initial ? 1 : 0;
    return f;
}
void gptps_flag_destroy(gptps_flag *f) { free(f); }
void gptps_flag_set(gptps_flag *f, bool value) { if (f) InterlockedExchange(&f->v, value ? 1 : 0); }
bool gptps_flag_get(const gptps_flag *f)
{
    /* full-barrier atomic read; InterlockedCompareExchange needs a non-const ptr */
    return f && InterlockedCompareExchange((volatile LONG *)&f->v, 0, 0) != 0;
}

/* --- mutex (CRITICAL_SECTION) -------------------------------------------- */
struct gptps_mutex { CRITICAL_SECTION cs; };

gptps_mutex *gptps_mutex_create(void)
{
    gptps_mutex *m = (gptps_mutex *)malloc(sizeof *m);
    if (m) InitializeCriticalSection(&m->cs);
    return m;
}
void gptps_mutex_destroy(gptps_mutex *m) { if (m) { DeleteCriticalSection(&m->cs); free(m); } }
void gptps_mutex_lock(gptps_mutex *m) { EnterCriticalSection(&m->cs); }
void gptps_mutex_unlock(gptps_mutex *m) { LeaveCriticalSection(&m->cs); }

/* --- condition variable -------------------------------------------------- */
struct gptps_cond { CONDITION_VARIABLE cv; };

gptps_cond *gptps_cond_create(void)
{
    gptps_cond *c = (gptps_cond *)malloc(sizeof *c);
    if (c) InitializeConditionVariable(&c->cv);
    return c;
}
void gptps_cond_destroy(gptps_cond *c) { free(c); } /* CONDITION_VARIABLE needs no teardown */
void gptps_cond_wait(gptps_cond *c, gptps_mutex *m) { SleepConditionVariableCS(&c->cv, &m->cs, INFINITE); }
void gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms)
{
    DWORD d = (ms > (uint64_t)(INFINITE - 1)) ? (INFINITE - 1) : (DWORD)ms;
    SleepConditionVariableCS(&c->cv, &m->cs, d); /* spurious/timeout wakeups: caller re-checks */
}
void gptps_cond_signal(gptps_cond *c) { WakeConditionVariable(&c->cv); }
void gptps_cond_broadcast(gptps_cond *c) { WakeAllConditionVariable(&c->cv); }

/* --- threads (_beginthreadex) -------------------------------------------- */
struct gptps_thread { HANDLE h; gptps_thread_fn fn; void *arg; };

static unsigned __stdcall win_thread_trampoline(void *p)
{
    gptps_thread *t = (gptps_thread *)p;
    t->fn(t->arg);   /* gptps_thread_fn returns void*; ignored, like pthreads */
    return 0;
}

gptps_thread *gptps_thread_start(gptps_thread_fn fn, void *arg)
{
    gptps_thread *t = (gptps_thread *)malloc(sizeof *t);
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    t->h = (HANDLE)_beginthreadex(NULL, 0, win_thread_trampoline, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}
void gptps_thread_join(gptps_thread *t)
{
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

uint64_t gptps_hal_thread_id(void) { return (uint64_t)GetCurrentThreadId(); }

/* Windows has no fork(), so there is no forked-child hazard to guard against. */
void     gptps_hal_fork_guard_install(void) { }
uint64_t gptps_hal_fork_generation(void) { return 0; }

/* --- dynamic loading ----------------------------------------------------- */
gptps_dl *gptps_dl_open(const char *path) { return (gptps_dl *)LoadLibraryA(path); }
void     *gptps_dl_sym(gptps_dl *h, const char *symbol) { return (void *)GetProcAddress((HMODULE)h, symbol); }
void      gptps_dl_close(gptps_dl *h) { if (h) FreeLibrary((HMODULE)h); }
/* The handle IS the HMODULE here - nothing was allocated to wrap it - so keeping
 * the mapping and dropping the bookkeeping is simply doing nothing. */
void      gptps_dl_release(gptps_dl *h) { (void)h; }

gptps_status gptps_hal_atomic_replace(const char *tmp_path, const char *final_path)
{ return MoveFileExA(tmp_path, final_path, MOVEFILE_REPLACE_EXISTING) ? GPTPS_OK : GPTPS_E_IO; }

#endif /* _WIN32 */
