/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_hal.h - GPTPS Hardware Abstraction Layer (INTERNAL, not a public API).
 *
 * The only platform-specific seam. The interface is pure C99; each
 * implementation (hal_posix.c, hal_win.c, freestanding/hal_stub.c, or a port built
 * in with -DGPTPS_HAL_SOURCE=<file>) uses the best primitive its platform offers.
 * Atomics live ONLY inside the implementation so the C99 core never includes an
 * _Atomic type.
 *
 *   core (C99) ──uses──► gptps_hal_* (this header) ──impl──► hal_posix.c / hal_win.c / ...
 *
 * The comments below are the CONTRACT a backend keeps. docs/HAL.md gives each clause
 * with the reason the core needs it, and tests/test_hal_conformance.c holds every
 * backend to it (CTest runs it against the HAL of every build). The contract also
 * leaves freedoms - spurious wakeups, a coarse clock, signals that wake more than
 * one - and tests/hal_chaos.c takes all of them while the suite runs on it (all but
 * the timing gates), which shows the core needs nothing more. The external-program executor is
 * platform code too but not HAL (exec_oop_posix.c / exec_win.c); the forked
 * EXEC_OOP kind is POSIX-only (no fork() on Windows).
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

/* --- hardware detection (feeds config auto-tune) -------------------------
 * GPTPS_OK with cpu_count >= 1 (ram_bytes 0 when unknown); NULL is GPTPS_E_INVAL. */
typedef struct {
    unsigned cpu_count;  /* online logical CPUs, always >= 1 */
    uint64_t ram_bytes;  /* total physical RAM; 0 if undetectable */
    bool     has_gpu;    /* NOT part of the contract: the core never reads it, and the
                          * bundled HALs leave it false. A GPU, like any device a task
                          * holds, is a named resource the host or the config file
                          * defines ([resources] gpu = 4). Kept for ABI layout. */
} gptps_hwinfo;

gptps_status gptps_hal_detect(gptps_hwinfo *out);

/* --- monotonic clock (milliseconds) --------------------------------------
 * Never decreases, on any thread, and runs at the rate of real time. Any step is
 * fine (Win32's is about 16 ms): the core never counts on two readings differing.
 * A HAL without a real-time clock (the freestanding stub counts reads) still runs
 * the engine, but its deadlines and backoff are then not in milliseconds. */
uint64_t gptps_hal_monotonic_ms(void);

/* --- cancel flag: NO LONGER CALLED BY THE CORE --------------------------- *
 * The core keeps each item's cancel flag inside the item, as a word it writes and
 * reads with gptps_hal_store_release_u32 / gptps_hal_load_acquire_u32 below, so a
 * submit allocates nothing in the HAL. These four stay declared so existing
 * backends still build; a new backend may leave them out.
 */
typedef struct gptps_flag gptps_flag;

gptps_flag *gptps_flag_create(bool initial);
void        gptps_flag_destroy(gptps_flag *f);
void        gptps_flag_set(gptps_flag *f, bool value);
bool        gptps_flag_get(const gptps_flag *f);

/* --- threads / mutex / condvar (dispatcher + worker pool) ---------------- *
 * Opaque + heap-allocated so the C99 core never embeds a pthread_t/Win32 type.
 *
 * mutex: mutual exclusion, and everything before an unlock is visible after the
 *   next lock. The core never locks a mutex it holds, so recursive (Win32's
 *   CRITICAL_SECTION) and non-recursive implementations both conform.
 * cond:  a wait releases the mutex while it blocks and holds it again when it
 *   returns. signal wakes at least one waiter, broadcast every current waiter;
 *   with no waiter both do nothing. A wait may return with no signal - the core
 *   re-checks every predicate in a loop - but not as a rule: a wait that returns
 *   at once nearly every time turns the worker pool and the dispatcher into
 *   spinning loops. timedwait returns after about `ms` with no signal, and early
 *   on one, for ANY ms up to UINT64_MAX: the dispatcher's sleep until the next
 *   deadline can be 4294967295000 ms (timeout_seconds * 1000). Clamp what the
 *   platform cannot express; the core works out its wait again on every wakeup.
 * thread: start runs fn(arg) on a new thread, or returns NULL. join waits for fn
 *   to return, frees the handle, and makes the thread's writes visible; once per
 *   thread. A thread may start late. A HAL that returns NULL from start runs the
 *   engine in MANUAL mode only (the freestanding stub).
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
 * distinct from every other live thread's - but it may be handed to a later thread
 * once this one has exited. The core compares it with the calling thread's for two
 * things: detecting re-entrancy - gptps_shutdown(), gptps_step() or a
 * gptps_unregister_task() that would have to wait, called from inside a task body
 * or a callback, which would otherwise join, free or wait on the very thread making
 * the call - and pinning an add-on's namespace window to the thread running its
 * setup(). For re-entrancy it keys a per-thread record by it (see the counter
 * below); it is never used for scheduling. A single-threaded HAL may return any
 * constant. */
uint64_t gptps_hal_thread_id(void);

/* A counter one thread updates with no lock, which a LATER thread may take over:
 * the per-thread callback depth the core keys by gptps_hal_thread_id. The record
 * passes to another thread when the OS gives it the old one's id, or when the core
 * hands an idle record (depth 0) to a new id. Loads acquire and stores release, so
 * whoever touches it next sees the last writer's store and everything before it.
 * A single-threaded HAL may use plain accesses. */
uint32_t gptps_hal_load_acquire_u32(const uint32_t *p);
void     gptps_hal_store_release_u32(uint32_t *p, uint32_t v);

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

/* --- dynamic loading (add-on loader) - OPTIONAL ---------------------------
 * open: NULL on failure. sym: NULL for a missing symbol. close: unload. A HAL
 * without dynamic loading returns NULL from open, and binary plug-ins (TOML
 * `addons = [...]`) are then unavailable. */
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

/* --- atomic file replace (settings save: temp -> final) - OPTIONAL -------- *
 * Atomically replace `final_path` with `tmp_path` (rename on POSIX, MoveFileEx
 * on Windows so it works when the target already exists). GPTPS_OK / GPTPS_E_IO.
 * A HAL without a filesystem returns GPTPS_E_IO, and settings save is then
 * unavailable. Recommended, not required: keep the replaced file's permissions, as
 * the bundled POSIX HAL does - save edits an operator's config file in place. */
gptps_status gptps_hal_atomic_replace(const char *tmp_path, const char *final_path);

/* --- still pending (later increment): OS memory cap (out-of-process
 * executor: setrlimit / cgroups / Job Objects).
 */

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_HAL_H */
