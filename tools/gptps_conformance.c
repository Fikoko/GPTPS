/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_conformance.c - prove a GPTPS binary plug-in before you ship it.
 *
 *   gptps_conformance <plugin.so> [--cycles N] [-v]
 *
 * Exit code = number of failed checks (0 = conformant).
 *
 * WHAT THIS EXISTS FOR, and why the core cannot do it:
 *
 * The engine always hands a plug-in its FULL host table, so the engine can never
 * discover that a plug-in reads past the table size it was given. But that is the
 * single most likely way a plug-in breaks in the field: it is built against a newer
 * GPTPS than the host it is dropped into, calls a routine the older host's table does
 * not contain, and jumps through whatever memory happens to lie past the end.
 *
 * This harness can test it because it LINKS libgptps and can therefore synthesise any
 * historical table shape out of public symbols - see the degradation ladder below.
 *
 * Deliberately carries its own 15-line dlopen shim rather than including
 * gptps_hal.h: the HAL is internal and is not installed, so the harness must build
 * against nothing but gptps.h and libgptps - exactly like the people who use it.
 * No third-party dependencies, same hand-rolled CHECK() as the test suite.
 */
#include "gptps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#if defined(_WIN32)
#  include <windows.h>
typedef HMODULE dl_t;
static dl_t  dl_open(const char *p) { return LoadLibraryA(p); }
static void *dl_sym(dl_t h, const char *s) { return (void *)GetProcAddress(h, s); }
static void  dl_close(dl_t h) { if (h) FreeLibrary(h); }
static const char *dl_error(void) { static char b[128]; snprintf(b, sizeof b, "error %lu", (unsigned long)GetLastError()); return b; }
#else
#  include <dlfcn.h>
typedef void *dl_t;
static dl_t  dl_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *dl_sym(dl_t h, const char *s) { return dlsym(h, s); }
static void  dl_close(dl_t h) { if (h) dlclose(h); }
static const char *dl_error(void) { const char *e = dlerror(); return e ? e : "(none)"; }
#endif

static int fails = 0, verbose = 0;
#define OK(fmt, ...)   do { printf("  ok    " fmt "\n", ##__VA_ARGS__); } while (0)
#define BAD(fmt, ...)  do { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); ++fails; } while (0)
#define FIX(fmt, ...)  do { printf("        fix: " fmt "\n", ##__VA_ARGS__); } while (0)
#define NOTE(fmt, ...) do { if (verbose) printf("        %s" fmt "\n", "", ##__VA_ARGS__); } while (0)

/* ---------------------------------------------------------------------------
 * The degradation ladder.
 *
 * Each rung is a host-table size that a REAL released core once had. A plug-in must
 * behave against every one of them: use api->struct_size to decide what it may call,
 * and degrade or refuse rather than read past the end.
 *
 * Slots past the rung hold POISON STUBS, not NULL. That matters. An older host's
 * table is genuinely SHORTER, so reading a later slot is undefined behaviour, which
 * makes a segfault the realistic outcome - and a harness that proves your bug by
 * crashing is a bad harness: it cannot say WHICH routine, it cannot continue to the
 * next rung, and it makes the CI leg that runs it look broken rather than
 * informative. So the table is allocated full-size and only its struct_size lies;
 * every slot past the rung records the violation and returns GPTPS_E_ABI.
 * ------------------------------------------------------------------------- */
static const char *g_poisoned;          /* first out-of-floor routine called */

#define POISON(field, ret, sig, args)                                   \
    static ret poison_##field sig {                                     \
        (void)sizeof(int args);                                         \
        if (!g_poisoned) g_poisoned = #field;                           \
        return (ret)0;                                                  \
    }

/* One stub per routine that can be past a rung. Adding a host-table routine without
 * adding a stub here means the new routine is silently untested by this ladder. */
static gptps_status p_register_constraint(gptps *e, gptps_constraint_fn f, void *u) { (void)e;(void)f;(void)u; if(!g_poisoned) g_poisoned="register_constraint"; return GPTPS_E_ABI; }
static gptps_status p_register_observer(gptps *e, gptps_event_cb f, void *u) { (void)e;(void)f;(void)u; if(!g_poisoned) g_poisoned="register_observer"; return GPTPS_E_ABI; }
static gptps_status p_register_setting(gptps *e, const gptps_setting_def *d) { (void)e;(void)d; if(!g_poisoned) g_poisoned="register_setting"; return GPTPS_E_ABI; }
static gptps_status p_unregister_task(gptps *e, const char *n, unsigned f) { (void)e;(void)n;(void)f; if(!g_poisoned) g_poisoned="unregister_task"; return GPTPS_E_ABI; }
static int          p_task_exists(gptps *e, const char *n) { (void)e;(void)n; if(!g_poisoned) g_poisoned="task_exists"; return 0; }
static gptps_status p_define_global(gptps *e, const char *k, gptps_setting_type t, const char *d, const char *c, unsigned f) { (void)e;(void)k;(void)t;(void)d;(void)c;(void)f; if(!g_poisoned) g_poisoned="define_global"; return GPTPS_E_ABI; }
static gptps_status p_define_task_setting(gptps *e, const char *l, gptps_setting_type t, const char *d, const char *c, unsigned f) { (void)e;(void)l;(void)t;(void)d;(void)c;(void)f; if(!g_poisoned) g_poisoned="define_task_setting"; return GPTPS_E_ABI; }
static gptps_status p_cancel(gptps *e, gptps_handle h) { (void)e;(void)h; if(!g_poisoned) g_poisoned="cancel"; return GPTPS_E_ABI; }
static gptps_status p_unregister_constraint(gptps *e, gptps_constraint_fn f, void *u) { (void)e;(void)f;(void)u; if(!g_poisoned) g_poisoned="unregister_constraint"; return GPTPS_E_ABI; }
static gptps_status p_unregister_observer(gptps *e, gptps_event_cb f, void *u) { (void)e;(void)f;(void)u; if(!g_poisoned) g_poisoned="unregister_observer"; return GPTPS_E_ABI; }
static gptps_status p_define_resource(gptps *e, const char *n, uint64_t b) { (void)e;(void)n;(void)b; if(!g_poisoned) g_poisoned="define_resource"; return GPTPS_E_ABI; }
static gptps_status p_set_task_resource_cost(gptps *e, const char *t, const char *r, uint64_t a) { (void)e;(void)t;(void)r;(void)a; if(!g_poisoned) g_poisoned="set_task_resource_cost"; return GPTPS_E_ABI; }
static gptps_status p_resource_usage(gptps *e, const char *n, uint64_t *r, uint64_t *b) { (void)e;(void)n;(void)r;(void)b; if(!g_poisoned) g_poisoned="resource_usage"; return GPTPS_E_ABI; }
static gptps_status p_set_scheduler(gptps *e, gptps_sched_fn f, void *u) { (void)e;(void)f;(void)u; if(!g_poisoned) g_poisoned="set_scheduler"; return GPTPS_E_ABI; }
static bool         p_is_cancelled(const gptps_ctx *c) { (void)c; if(!g_poisoned) g_poisoned="is_cancelled"; return false; }
static uint64_t     p_deadline_ms(const gptps_ctx *c) { (void)c; if(!g_poisoned) g_poisoned="deadline_ms"; return 0; }
static uint64_t     p_now_ms(const gptps_ctx *c) { (void)c; if(!g_poisoned) g_poisoned="now_ms"; return 0; }
static gptps_status p_result_set_nocopy(gptps_ctx *c, void *b, size_t n, void (*f)(void *)) { (void)c;(void)b;(void)n;(void)f; if(!g_poisoned) g_poisoned="result_set_nocopy"; return GPTPS_E_ABI; }
static gptps_status p_task_setting_int(gptps_ctx *c, const char *k, long *o) { (void)c;(void)k;(void)o; if(!g_poisoned) g_poisoned="task_setting_int"; return GPTPS_E_ABI; }
static gptps_status p_task_setting_str(gptps_ctx *c, const char *k, char *b, size_t n) { (void)c;(void)k;(void)b;(void)n; if(!g_poisoned) g_poisoned="task_setting_str"; return GPTPS_E_ABI; }
static gptps_status p_submit(gptps *e, const char *t, const void *p, size_t n, gptps_handle *h) { (void)e;(void)t;(void)p;(void)n;(void)h; if(!g_poisoned) g_poisoned="submit"; return GPTPS_E_ABI; }
static gptps_status p_submit_ex(gptps *e, const char *t, const void *p, size_t n, const gptps_submit_options *o, gptps_handle *h) { (void)e;(void)t;(void)p;(void)n;(void)o;(void)h; if(!g_poisoned) g_poisoned="submit_ex"; return GPTPS_E_ABI; }
static gptps_status p_settings_get(gptps *e, const char *k, char *b, size_t c) { (void)e;(void)k;(void)b;(void)c; if(!g_poisoned) g_poisoned="settings_get"; return GPTPS_E_ABI; }
static gptps_status p_settings_set(gptps *e, const char *k, const char *v) { (void)e;(void)k;(void)v; if(!g_poisoned) g_poisoned="settings_set"; return GPTPS_E_ABI; }
static gptps_status p_settings_watch(gptps *e, gptps_settings_cb c, void *u) { (void)e;(void)c;(void)u; if(!g_poisoned) g_poisoned="settings_watch"; return GPTPS_E_ABI; }
static gptps_status p_set_task_priority(gptps *e, const char *t, int p) { (void)e;(void)t;(void)p; if(!g_poisoned) g_poisoned="set_task_priority"; return GPTPS_E_ABI; }
static const char  *p_strerror(gptps_status s) { (void)s; if(!g_poisoned) g_poisoned="strerror"; return "?"; }
static const char  *p_version(void) { if(!g_poisoned) g_poisoned="version"; return "?"; }
static gptps_status p_set_scheduler_ex(gptps *e, gptps_sched_fn f, void *u, const char *o, unsigned fl) { (void)e;(void)f;(void)u;(void)o;(void)fl; if(!g_poisoned) g_poisoned="set_scheduler_ex"; return GPTPS_E_ABI; }

/* emit_event has no public symbol (it is internal to the engine), so the harness
 * supplies a benign stand-in rather than the real thing. A plug-in that emits an
 * event during setup() sees success and no event; that is a harness limitation, not
 * a conformance verdict, and it is the only slot with one. */
static gptps_status h_emit_event(gptps *e, const gptps_event *ev) { (void)e; (void)ev; return GPTPS_OK; }

typedef struct { const char *label; size_t size; } rung;

static void build_table(gptps_api_routines *t, size_t rung_size)
{
    memset(t, 0, sizeof *t);
    t->struct_size       = rung_size;         /* the only thing that "lies" */
    t->abi_version_major = GPTPS_ABI_VERSION_MAJOR;
    t->abi_version_minor = GPTPS_ABI_VERSION_MINOR;

    /* v1.0 - always present, in every core that ever shipped */
    t->register_task = gptps_register_task;
    t->emit_event    = h_emit_event;
    t->log           = gptps_log;
    t->result_set    = gptps_result_set;
    t->payload       = gptps_payload;

#define SLOT(field, real, poison) \
    t->field = (rung_size > offsetof(gptps_api_routines, field)) ? (real) : (poison)

    SLOT(register_constraint,     gptps_register_constraint,     p_register_constraint);
    SLOT(register_observer,       gptps_register_observer,       p_register_observer);
    SLOT(register_setting,        gptps_register_setting,        p_register_setting);
    SLOT(unregister_task,         gptps_unregister_task,         p_unregister_task);
    SLOT(task_exists,             gptps_task_exists,             p_task_exists);
    SLOT(define_global,           gptps_define_global,           p_define_global);
    SLOT(define_task_setting,     gptps_define_task_setting,     p_define_task_setting);
    SLOT(cancel,                  gptps_cancel,                  p_cancel);
    SLOT(unregister_constraint,   gptps_unregister_constraint,   p_unregister_constraint);
    SLOT(unregister_observer,     gptps_unregister_observer,     p_unregister_observer);
    SLOT(define_resource,         gptps_define_resource,         p_define_resource);
    SLOT(set_task_resource_cost,  gptps_set_task_resource_cost,  p_set_task_resource_cost);
    SLOT(resource_usage,          gptps_resource_usage,          p_resource_usage);
    SLOT(set_scheduler,           gptps_set_scheduler,           p_set_scheduler);
    SLOT(is_cancelled,            gptps_is_cancelled,            p_is_cancelled);
    SLOT(deadline_ms,             gptps_deadline_ms,             p_deadline_ms);
    SLOT(now_ms,                  gptps_now_ms,                  p_now_ms);
    SLOT(result_set_nocopy,       gptps_result_set_nocopy,       p_result_set_nocopy);
    SLOT(task_setting_int,        gptps_task_setting_int,        p_task_setting_int);
    SLOT(task_setting_str,        gptps_task_setting_str,        p_task_setting_str);
    SLOT(submit,                  gptps_submit,                  p_submit);
    SLOT(submit_ex,               gptps_submit_ex,               p_submit_ex);
    SLOT(settings_get,            gptps_settings_get,            p_settings_get);
    SLOT(settings_set,            gptps_settings_set,            p_settings_set);
    SLOT(settings_watch,          gptps_settings_watch,          p_settings_watch);
    SLOT(set_task_priority,       gptps_set_task_priority,       p_set_task_priority);
    SLOT(strerror_fn,             gptps_strerror,                p_strerror);
    SLOT(version,                 gptps_version,                 p_version);
    SLOT(set_scheduler_ex,        gptps_set_scheduler_ex,        p_set_scheduler_ex);
#undef SLOT
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int cycles = 3, i, c;
    dl_t h;
    void *sym;
    gptps_addon_init_fn init;
    const gptps_addon *a;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) cycles = atoi(argv[++i]);
        else path = argv[i];
    }
    if (!path) {
        printf("usage: gptps_conformance <plugin.so> [--cycles N] [-v]\n");
        return 2;
    }
    printf("gptps_conformance: %s\n", path);
    printf("host: gptps %s, ABI %u.%u\n\n", gptps_version(),
           GPTPS_ABI_VERSION_MAJOR, GPTPS_ABI_VERSION_MINOR);

    /* C1 - it loads at all */
    h = dl_open(path);
    if (!h) { BAD("C1  dlopen: %s", dl_error()); FIX("wrong architecture, a missing transitive .so, or not a shared library"); return fails; }
    OK("C1  loads");

    /* C2 - it exports the one symbol that is the whole ABI surface */
    sym = dl_sym(h, "gptps_addon_init");
    if (!sym) { BAD("C2  no exported gptps_addon_init"); FIX("use GPTPS_ADDON_INIT/-_NS, and do not hide it with -fvisibility=hidden without the export attribute"); dl_close(h); return fails; }
    memcpy(&init, &sym, sizeof init);
    OK("C2  exports gptps_addon_init");

    /* C3 - the descriptor gate, FIELD BY FIELD.
     * The core reports one opaque GPTPS_E_ABI for any of these; naming the actual
     * field is the single most useful thing this tool does. */
    {
        gptps_api_routines full;
        build_table(&full, sizeof full);
        a = init(&full);
        if (!a) { BAD("C3  gptps_addon_init returned NULL"); dl_close(h); return fails; }
        if (a->magic != GPTPS_ABI_MAGIC)
            { BAD("C3  magic is 0x%08X, expected 0x%08X", a->magic, GPTPS_ABI_MAGIC); FIX("build against this gptps.h; do not hand-roll the descriptor"); }
        else OK("C3  magic");
        if (a->abi_version_major != GPTPS_ABI_VERSION_MAJOR)
            { BAD("C3  abi_version_major is %u, this host is %u", a->abi_version_major, GPTPS_ABI_VERSION_MAJOR); FIX("a MAJOR mismatch is never loadable - rebuild against this core"); }
        else OK("C3  abi_version_major");
        if (a->struct_size < offsetof(gptps_addon, teardown) + sizeof a->teardown) {
            /* TERMINAL. Everything below reads further into the descriptor - name,
             * setup, ns - and a struct_size below the floor means those fields may
             * not be there. Reporting and continuing walked off the end of a
             * truncated descriptor, which is the exact class of bug this tool
             * exists to catch, committed by the tool. */
            BAD("C3  struct_size %u is below the frozen floor (%u)",
                (unsigned)a->struct_size,
                (unsigned)(offsetof(gptps_addon, teardown) + sizeof a->teardown));
            FIX("set struct_size = sizeof(gptps_addon); use GPTPS_ADDON_INIT/_NS");
            dl_close(h);
            printf("\nNOT CONFORMANT: %d check(s) failed\n", fails);
            return fails;
        }
        OK("C3  struct_size (%u bytes)", (unsigned)a->struct_size);
        if (a->struct_size > sizeof(gptps_addon))
            NOTE("built against a NEWER minor than this host; trailing fields ignored");
        if (!a->name) { BAD("C3  name is NULL"); FIX("the loader rejects this, and introspection reports it"); }
        else OK("C3  name = \"%s\"", a->name);
        if (!a->setup) NOTE("no setup() - the add-on registers nothing");

        if (a->struct_size >= offsetof(gptps_addon, ns) + sizeof a->ns && a->ns)
            OK("C4  namespace = \"%s\" (claimed; nothing else may take it)", a->ns);
        else
            NOTE("C4  unnamespaced - fine, but out of tree a namespace prevents collisions");
    }

    /* C5 - it loads through the REAL loader, so the harness's own gate and the
     * core's agree. C6 - and it actually registered something. */
    {
        gptps *e = NULL;
        size_t t0, t1, s0, s1;
        if (gptps_open(NULL, &e) != GPTPS_OK || !e) { BAD("C5  could not open an engine"); dl_close(h); return fails; }
        t0 = gptps_task_count(e); s0 = gptps_settings_count(e);
        {
            gptps_status st = gptps_load_addon(e, path);
            if (st != GPTPS_OK) { BAD("C5  gptps_load_addon -> %s", gptps_strerror(st)); FIX("see the C3 lines above for which field the gate rejected"); }
            else OK("C5  loads through gptps_load_addon");
        }
        t1 = gptps_task_count(e); s1 = gptps_settings_count(e);
        if (t1 == t0 && s1 == s0) { BAD("C6  setup() registered no task and no setting"); FIX("a plug-in that registers nothing has no effect - check setup()'s return"); }
        else OK("C6  registered %u task(s), %u setting(s)", (unsigned)(t1 - t0), (unsigned)(s1 - s0));

        /* C7 - every newly-registered task must actually reach a terminal state.
         * Needs no configuration from the author: submit one empty item to each. */
        {
            size_t k; int ran = 0;
            for (k = 0; k < gptps_task_count(e); ++k) {
                gptps_task_info ti; gptps_handle hh = 0;
                memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
                if (gptps_task_get_info(e, k, &ti) != GPTPS_OK || !ti.name) continue;
                if (gptps_submit(e, ti.name, NULL, 0, &hh) == GPTPS_OK) ++ran;
            }
            if (ran) OK("C7  submitted %d item(s) to the add-on's task types", ran);
        }
        gptps_shutdown(e);   /* must RETURN - a task that ignores cancellation hangs here */
        OK("C7  gptps_shutdown returned (no task ignored cancellation)");
    }

    /* C10 - the degradation ladder. */
    {
        rung rungs[] = {
            { "v1.0",  offsetof(gptps_api_routines, register_constraint) },
            { "v1.1",  offsetof(gptps_api_routines, register_setting) },
            { "v1.4",  offsetof(gptps_api_routines, unregister_task) },
            { "v1.8",  offsetof(gptps_api_routines, cancel) },
            { "v1.9",  offsetof(gptps_api_routines, define_resource) },
            { "v1.10", offsetof(gptps_api_routines, set_scheduler) },
            { "v1.12", offsetof(gptps_api_routines, is_cancelled) },
            { "2.1",   sizeof(gptps_api_routines) },
        };
        size_t n = sizeof rungs / sizeof rungs[0];
        for (i = 0; i < (int)n; ++i) {
            gptps_api_routines t;
            gptps *e = NULL;
            const gptps_addon *ad;
            gptps_status st;
            const char *where = "setup()";
            g_poisoned = NULL;
            build_table(&t, rungs[i].size);
            if (gptps_open(NULL, &e) != GPTPS_OK || !e) continue;
            gptps_settings_set(e, "limits.shutdown_grace_ms", "500");
            ad = init(&t);
            st = (ad && ad->setup) ? ad->setup(e, &t, NULL) : GPTPS_OK;

            /* RUN the tasks too, not just setup().
             *
             * A plug-in's task BODY is the likeliest place to call an unguarded
             * routine - is_cancelled above all, which is exactly what a looping task
             * needs and exactly what an older host does not have. Checking only
             * setup() certified this tool's own reference template as conformant
             * while its run() called is_cancelled unguarded. Submit one item to every
             * task the add-on registered and drain, so run() executes against the
             * degraded table before the verdict. */
            if (!g_poisoned && st == GPTPS_OK) {
                size_t k;
                uint64_t t0;
                for (k = 0; k < gptps_task_count(e); ++k) {
                    gptps_task_info ti; gptps_handle hh = 0;
                    memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
                    if (gptps_task_get_info(e, k, &ti) != GPTPS_OK || !ti.name) continue;
                    (void)gptps_submit(e, ti.name, NULL, 0, &hh);
                }
                /* Wait for the work to finish HERE rather than letting shutdown do it,
                 * because teardown() must run while the engine is still alive. */
                t0 = gptps_now_ms(NULL);
                for (;;) {
                    size_t k2, busy = 0;
                    for (k2 = 0; k2 < gptps_task_count(e); ++k2) {
                        gptps_task_info ti;
                        memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
                        if (gptps_task_get_info(e, k2, &ti) == GPTPS_OK) busy += ti.queued + ti.running;
                    }
                    if (!busy || gptps_now_ms(NULL) - t0 > 3000) break;
                }
                if (g_poisoned) where = "run()";
            }

            /* teardown() is part of the contract and may call the table too. It runs
             * BEFORE gptps_shutdown, because shutdown frees the engine - and because
             * this harness calls setup() directly rather than through
             * gptps_load_addon, so the engine does not know to call teardown itself.
             * Running it also stops this tool leaking an engine's worth of add-on
             * state per rung and then telling the author to go run LeakSanitizer. */
            if (ad && ad->teardown) {
                const char *before = g_poisoned;
                ad->teardown(e);
                if (!before && g_poisoned) where = "teardown()";
            }
            gptps_shutdown(e);

            if (g_poisoned)
                { BAD("C10 rung %-5s called api->%s from %s, past its declared table size",
                      rungs[i].label, g_poisoned, where);
                  FIX("guard it: if (api->struct_size <= offsetof(gptps_api_routines, %s)) ...", g_poisoned); }
            else if (st == GPTPS_OK)
                OK("C10 rung %-5s setup + run + teardown stayed inside the table given", rungs[i].label);
            else
                OK("C10 rung %-5s setup() declined cleanly (%s) - correct", rungs[i].label, gptps_strerror(st));
        }
    }

    /* C11 - repeated load/run/shutdown in ONE process, to catch non-reentrant
     * globals and a teardown that frees twice. */
    for (c = 0; c < cycles; ++c) {
        gptps *e = NULL;
        if (gptps_open(NULL, &e) != GPTPS_OK || !e) break;
        if (gptps_load_addon(e, path) != GPTPS_OK) { BAD("C11 load failed on cycle %d of %d", c + 1, cycles); gptps_shutdown(e); break; }
        gptps_shutdown(e);
    }
    if (c == cycles) OK("C11 survived %d load/shutdown cycles in one process", cycles);

    dl_close(h);
    printf("\n%s: %d check(s) failed\n", fails ? "NOT CONFORMANT" : "CONFORMANT", fails);
    if (!fails) printf("tip: re-run under `ASAN_OPTIONS=detect_leaks=1` with an ASan build to catch leaks.\n");
    return fails;
}
