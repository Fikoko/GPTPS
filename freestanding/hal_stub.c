/*
 * hal_stub.c - a freestanding/MANUAL-mode reference HAL backend.
 *
 * The HAL (gptps_hal.h) is GPTPS's only platform seam. This stub implements the
 * full interface with NO operating system: no pthreads, no dlopen, no
 * filesystem, no real-time clock. All memory comes from a fixed static arena, so
 * a build using this backend needs no libc heap. It is sufficient to run the
 * engine in MANUAL mode (gptps_step) with in-process tasks - which is exactly
 * the "one task, any hardware" / bare-metal claim, demonstrated end to end by
 * demo_freestanding.c. Threaded-mode primitives exist only to satisfy the link;
 * MANUAL mode never calls them.
 */
#include "gptps_hal.h"

/* --- static bump arena (no libc heap) ------------------------------------ */
static union { unsigned char b[64 * 1024]; long double align_; } g_arena;
static size_t g_off = 0;

static void *arena_alloc(size_t n)
{
    void *p;
    n = (n + 15u) & ~(size_t)15u;
    if (g_off + n > sizeof g_arena.b) return 0;     /* arena exhausted */
    p = &g_arena.b[g_off];
    g_off += n;
    return p;
}

/* --- hardware detection: single CPU, RAM unknown -------------------------- */
gptps_status gptps_hal_detect(gptps_hwinfo *out)
{
    if (!out) return GPTPS_E_INVAL;
    out->cpu_count = 1u; out->ram_bytes = 0; out->has_gpu = false;
    return GPTPS_OK;
}

/* --- monotonic clock: a counter (no RTC on bare metal) -------------------- */
static uint64_t g_ticks = 0;
uint64_t gptps_hal_monotonic_ms(void) { return ++g_ticks; }

/* --- cancel flag: a byte in the arena ------------------------------------- */
struct gptps_flag { volatile int v; };
gptps_flag *gptps_flag_create(bool initial)
{ struct gptps_flag *f = (struct gptps_flag *)arena_alloc(sizeof *f); if (f) f->v = initial ? 1 : 0; return f; }
void gptps_flag_destroy(gptps_flag *f) { (void)f; }              /* arena: no per-item free */
void gptps_flag_set(gptps_flag *f, bool value) { if (f) f->v = value ? 1 : 0; }
bool gptps_flag_get(const gptps_flag *f) { return f && f->v != 0; }

/* --- mutex / cond: no-ops (MANUAL mode is single-threaded) ---------------- */
struct gptps_mutex { int _; };
struct gptps_cond  { int _; };
static struct gptps_mutex g_one_mutex;
static struct gptps_cond  g_one_cond;
gptps_mutex *gptps_mutex_create(void) { return &g_one_mutex; }
void gptps_mutex_destroy(gptps_mutex *m) { (void)m; }
void gptps_mutex_lock(gptps_mutex *m) { (void)m; }
void gptps_mutex_unlock(gptps_mutex *m) { (void)m; }
gptps_cond *gptps_cond_create(void) { return &g_one_cond; }
void gptps_cond_destroy(gptps_cond *c) { (void)c; }
void gptps_cond_wait(gptps_cond *c, gptps_mutex *m) { (void)c; (void)m; }
void gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms) { (void)c; (void)m; (void)ms; }
void gptps_cond_signal(gptps_cond *c) { (void)c; }
void gptps_cond_broadcast(gptps_cond *c) { (void)c; }

/* --- threads: never started in MANUAL mode; present only to link ---------- */
gptps_thread *gptps_thread_start(gptps_thread_fn fn, void *arg) { (void)fn; (void)arg; return 0; }
void gptps_thread_join(gptps_thread *t) { (void)t; }

/* --- dynamic loading: no add-ons on bare metal ---------------------------- */
gptps_dl *gptps_dl_open(const char *path) { (void)path; return 0; }
void     *gptps_dl_sym(gptps_dl *h, const char *symbol) { (void)h; (void)symbol; return 0; }
void      gptps_dl_close(gptps_dl *h) { (void)h; }

/* --- atomic file replace: no filesystem ----------------------------------- */
gptps_status gptps_hal_atomic_replace(const char *tmp_path, const char *final_path)
{ (void)tmp_path; (void)final_path; return GPTPS_E_IO; }
