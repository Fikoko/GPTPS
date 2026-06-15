/*
 * gptps_hal.h - GPTPS Hardware Abstraction Layer (INTERNAL, not a public API).
 *
 * The only platform-specific seam. The interface is pure C99; each
 * implementation (hal_posix.c, later hal_win.c) uses the best primitive its
 * platform/toolchain offers. Atomics live ONLY inside the implementation so
 * the C99 core never includes an _Atomic type.
 *
 *   core (C99) ──uses──► gptps_hal_* (this header) ──impl──► hal_posix.c / hal_win.c
 *
 * Build status: hwdetect, monotonic clock, and the cancel flag are implemented
 * (T3, first slice). Threads, dynamic loading, and the OS memory cap are part
 * of this seam and land in later T3 increments.
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

/* --- still pending (later increments): dynamic loading (add-on loader),
 * OS memory cap (out-of-process executor: setrlimit / cgroups / Job Objects).
 */

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_HAL_H */
