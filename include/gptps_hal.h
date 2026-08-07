/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_hal.h - GPTPS Hardware Abstraction Layer (INTERNAL, not a public API).
 *
 * The only platform-specific seam. The interface is pure C99; each
 * implementation (hal_posix.c, hal_win.c) uses the best primitive its
 * platform/toolchain offers. Atomics live ONLY inside the implementation so
 * the C99 core never includes an _Atomic type.
 *
 *   core (C99) ──uses──► gptps_hal_* (this header) ──impl──► hal_posix.c / hal_win.c
 *
 * Both backends implement the full interface (hwdetect, monotonic clock, cancel
 * flag, threads/mutex/condvar, dynamic loading). The external-program executor
 * exists on both (exec_oop_posix.c / exec_win.c via CreateProcess + Job Object);
 * the forked EXEC_OOP kind is POSIX-only (no fork() on Windows).
 */
#ifndef GPTPS_HAL_H
#define GPTPS_HAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "gptps.h" /* gptps_status */

#ifdef __cplusplus
extern "C" {
#endif

/* --- hardware detection (feeds config auto-tune) ------------------------- */
typedef struct {
    unsigned cpu_count;  /* online logical CPUs, always >= 1 */
    uint64_t ram_bytes;  /* total physical RAM; 0 if undetectable */
    bool     has_gpu;    /* best-effort; false when unknown (needs a GPU add-on) */
} gptps_hwinfo;

gptps_status gptps_hal_detect(gptps_hwinfo *out);

/* --- monotonic clock (milliseconds) -------------------------------------- */
uint64_t gptps_hal_monotonic_ms(void);

/* --- cancel flag --------------------------------------------------------- *
 * Opaque + heap-allocated so the _Atomic / __atomic storage stays confined to
 * the HAL implementation. The watchdog thread calls _set(); the task thread
 * polls via gptps_is_cancelled() which reads _get(). Correct under the C
 * memory model on the real path (compiler atomics); see hal_posix.c for the
 * pre-builtin fallback caveat.
 */
typedef struct gptps_flag gptps_flag;

gptps_flag *gptps_flag_create(bool initial);
void        gptps_flag_destroy(gptps_flag *f);
void        gptps_flag_set(gptps_flag *f, bool value);
bool        gptps_flag_get(const gptps_flag *f);

/* --- threads / mutex / condvar (dispatcher + worker pool) ---------------- *
 * Opaque + heap-allocated so the C99 core never embeds a pthread_t/Win32 type.
 */
typedef struct gptps_mutex  gptps_mutex;
typedef struct gptps_cond   gptps_cond;
typedef struct gptps_thread gptps_thread;
typedef void *(*gptps_thread_fn)(void *arg);

gptps_mutex *gptps_mutex_create(void);
void         gptps_mutex_destroy(gptps_mutex *m);
void         gptps_mutex_lock(gptps_mutex *m);
void         gptps_mutex_unlock(gptps_mutex *m);

gptps_cond *gptps_cond_create(void);
void        gptps_cond_destroy(gptps_cond *c);
void        gptps_cond_wait(gptps_cond *c, gptps_mutex *m);
void        gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms); /* wakes after ~ms or on signal */
void        gptps_cond_signal(gptps_cond *c);
void        gptps_cond_broadcast(gptps_cond *c);

gptps_thread *gptps_thread_start(gptps_thread_fn fn, void *arg); /* NULL on failure */
void          gptps_thread_join(gptps_thread *t);               /* joins, then frees */

/* An opaque, comparable id for the CALLING thread, stable for its lifetime and
 * distinct from every other live thread's. The core uses it for ONE purpose:
 * detecting re-entrancy - gptps_shutdown() or gptps_step() called from inside a
 * task body or an event callback, which would otherwise join (or free) the very
 * thread making the call. Never used for scheduling, indexing, or storage.
 * A single-threaded HAL may return any constant. */
uint64_t gptps_hal_thread_id(void);

/* --- fork safety (POSIX; a no-op elsewhere) ------------------------------ *
 * A host that fork()s while worker threads are live gets a child where only the
 * calling thread exists but the engine mutex may still be LOCKED by a thread that
 * did not survive - so the child deadlocks on its first call into that engine.
 * POSIX allows only async-signal-safe calls in such a child anyway, so the
 * contract is: an engine created BEFORE a fork must not be used after it.
 *
 * The generation counter makes exactly that distinguishable. It increments in the
 * child on every fork; an engine stamps it at creation and compares on entry, so
 * an INHERITED engine is refused (GPTPS_E_SHUTDOWN) while an engine created fresh
 * in the child - the fork-a-worker-process pattern, e.g. addons/gptps_xport - is
 * untouched. Install is idempotent. A HAL with no fork (Win32, freestanding)
 * installs nothing and returns a constant, compiling the check away. */
void     gptps_hal_fork_guard_install(void);
uint64_t gptps_hal_fork_generation(void);

/* --- dynamic loading (add-on loader) ------------------------------------ */
typedef struct gptps_dl gptps_dl;
gptps_dl *gptps_dl_open(const char *path);          /* RTLD_LOCAL; NULL on failure */
void     *gptps_dl_sym(gptps_dl *h, const char *symbol);
void      gptps_dl_close(gptps_dl *h);
/* Free the HANDLE's bookkeeping WITHOUT unloading the library.
 *
 * For the one case the add-on loader genuinely needs: a setup() that failed partway
 * may have left pointers the unwind cannot reach (a settings entry's read/write
 * pair, a per-task setting schema), so unmapping would turn each of those into a
 * wild jump. Retaining the MAPPING is the deliberate trade there - but retaining
 * this small wrapper too is not, since nothing references it once the load has
 * failed. Separating the two makes the intent exact and keeps the failure path
 * leak-free under LeakSanitizer. */
void      gptps_dl_release(gptps_dl *h);

/* --- atomic file replace (settings save: temp -> final) ------------------ *
 * Atomically replace `final_path` with `tmp_path` (rename on POSIX, MoveFileEx
 * on Windows so it works when the target already exists). GPTPS_OK / GPTPS_E_IO. */
gptps_status gptps_hal_atomic_replace(const char *tmp_path, const char *final_path);

/* --- still pending (later increment): OS memory cap (out-of-process
 * executor: setrlimit / cgroups / Job Objects).
 */

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_HAL_H */
