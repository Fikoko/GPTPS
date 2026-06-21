/*
 * test_tui.c - terminal dashboard add-on, verified HEADLESSLY (no TTY): drive the
 * engine + the dashboard's key handling, then assert on the rendered frame string
 * and on independently-counted events. The live loop (gptps_tui_run) and raw
 * terminal I/O are exercised only on a real terminal and are not unit-tested here.
 */
#include "gptps.h"
#include "tui.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static int c_done;   /* independent observer: counts finishes of "echo" */

static void count_obs(const gptps_event *ev, void *ud)
{ (void)ud; if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "echo") == 0) inc(&c_done); }

static gptps_status task_ok(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

int main(void)
{
    gptps *e = NULL;
    gptps_tui *t;
    gptps_tui_config cfg;
    gptps_task_def d;
    gptps_handle h;
    char frame[8192];
    int i;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return 1;
    gptps_register_observer(e, count_obs, NULL);   /* independent of the dashboard */

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.color = 0;            /* plain text so assertions match exactly */
    cfg.interactive = 0;      /* no TTY needed */
    cfg.title = "demo board";
    cfg.settings_path = "tui_settings.toml";   /* where the pane's 'w' saves */
    t = gptps_tui_install(e, &cfg);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return 1; }

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "echo"; d.run = task_ok; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    /* local/per-task config: label + hotkey 'e' submits "echo" */
    CHECK(gptps_tui_add_task(t, "echo", "Echo", 'e', NULL, 0) == GPTPS_OK);

    /* key handling (the interactivity contract) */
    CHECK(gptps_tui_press(t, 'e') == 1);   /* bound hotkey -> submits */
    CHECK(gptps_tui_press(t, 'e') == 1);
    CHECK(gptps_tui_press(t, 'z') == 0);   /* unbound -> ignored */
    /* plus a couple of direct submits */
    CHECK(gptps_submit(e, "echo", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "echo", NULL, 0, &h) == GPTPS_OK);

    /* wait until all four finish (two hotkey + two direct) */
    { uint64_t start = gptps_now_ms(NULL);
      while (get(&c_done) < 4 && gptps_now_ms(NULL) - start < 2000) { } }
    CHECK(get(&c_done) == 4);

    /* render a frame and assert its content reflects the live state */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strlen(frame) > 0);
    CHECK(strstr(frame, "demo board") != NULL);   /* global title */
    CHECK(strstr(frame, "Echo")       != NULL);   /* local label */
    CHECK(strstr(frame, "[e]")        != NULL);   /* hotkey legend */
    CHECK(strstr(frame, "TASKS")      != NULL);
    CHECK(strstr(frame, "RECENT")     != NULL);
    CHECK(strstr(frame, "FINISHED")   != NULL);   /* a recent finished event */
    CHECK(strstr(frame, "finished 4") != NULL);   /* cumulative count */
    CHECK(strstr(frame, "done/s")     != NULL);   /* throughput metric */
    CHECK(strstr(frame, "avg ms")     != NULL);   /* latency column */
    CHECK(strstr(frame, "ok%")        != NULL);   /* success-rate column */
    CHECK(strstr(frame, "in-flight")  != NULL);   /* gauge */
    CHECK(strstr(frame, "\x1b[") == NULL);        /* color off => no escape codes */

    /* keyboard scroll (k = older, j = newer); not bound to a task => return 2 */
    CHECK(gptps_tui_press(t, 'k') == 2);
    CHECK(gptps_tui_press(t, 'j') == 2);

    /* runtime reconfiguration: dial the KPI cost down (drops panes + frees the
     * latency ring), then back up; both visible in the next rendered frame */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_MINIMAL) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "kpi:minimal") != NULL);
    CHECK(strstr(frame, "TASKS")       == NULL);   /* panes dropped */
    CHECK(strstr(frame, "RECENT")      == NULL);
    CHECK(strstr(frame, "finished 4")  != NULL);   /* counts still tracked */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_FULL) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "kpi:full") != NULL);
    CHECK(strstr(frame, "TASKS")    != NULL);
    CHECK(strstr(frame, "avg ms")   != NULL);

    /* the unified settings registry drives the dashboard (tui.* settings) */
    {
        char b[GPTPS_SETTINGS_VALUE_MAX];
        CHECK(gptps_settings_get(e, "tui.kpi", b, sizeof b) == GPTPS_OK && strcmp(b, "full") == 0);
        CHECK(gptps_settings_set(e, "tui.kpi", "minimal") == GPTPS_OK);
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "kpi:minimal") != NULL && strstr(frame, "TASKS") == NULL);
        CHECK(gptps_settings_set(e, "tui.kpi", "bogus") == GPTPS_E_CONFIG);   /* enum validated */
        CHECK(gptps_settings_set(e, "tui.kpi", "full") == GPTPS_OK);          /* restore */
    }

    /* mode + refresh, reflected live in the status line */
    CHECK(gptps_tui_set_mode(t, GPTPS_TUI_ON_DEMAND) == GPTPS_OK);
    CHECK(gptps_tui_set_refresh(t, 500) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "mode:on-demand") != NULL);
    CHECK(strstr(frame, "refresh:500ms")  != NULL);

    /* 'm' cycles the KPI level at runtime (returns the "reconfigured" code) */
    CHECK(gptps_tui_press(t, 'm') == 3);

    /* snapshot ("once" / on demand) writes a frame to the configured stream */
    {
        FILE *f = fopen("tui_snap.txt", "w+");
        if (f) {
            gptps *es = NULL; gptps_tui_config cs; gptps_tui *ts;
            memset(&cs, 0, sizeof cs); cs.struct_size = sizeof cs;
            cs.color = 0; cs.interactive = 0; cs.title = "snap"; cs.out = f;
            CHECK(gptps_open(NULL, &es) == GPTPS_OK);
            ts = gptps_tui_install(es, &cs);
            if (ts) {
                char rd[2048]; long n;
                gptps_tui_snapshot(ts); fflush(f);
                fseek(f, 0, SEEK_SET); n = (long)fread(rd, 1, sizeof rd - 1, f); rd[n > 0 ? n : 0] = 0;
                CHECK(strstr(rd, "snap") != NULL);
                gptps_shutdown(es); gptps_tui_close(ts);
            } else { gptps_shutdown(es); }
            fclose(f); remove("tui_snap.txt");
        }
    }

    /* hiding a pane (global setting) */
    {
        gptps *e2 = NULL; gptps_tui *t2; gptps_tui_config c2;
        memset(&c2, 0, sizeof c2); c2.struct_size = sizeof c2;
        c2.color = 0; c2.interactive = 0; c2.title = "x"; c2.show_recent = -1; /* hide recent */
        CHECK(gptps_open(NULL, &e2) == GPTPS_OK);
        t2 = gptps_tui_install(e2, &c2);
        if (t2) {
            char f2[2048]; gptps_tui_render(t2, f2, sizeof f2);
            CHECK(strstr(f2, "RECENT") == NULL);   /* hidden */
            CHECK(strstr(f2, "TASKS")  != NULL);   /* still shown */
            gptps_shutdown(e2);
            gptps_tui_close(t2);
        } else { gptps_shutdown(e2); }
    }

    /* ---- Settings pane: open, navigate, edit inline, save (headless) ---- */
    {
        int guard;
        CHECK(gptps_tui_press(t, 's') == 4);             /* open the pane */
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "settings") != NULL);
        CHECK(strstr(frame, "scheduler.reserve_after_skips") != NULL);
        /* navigate (j) until the scheduler knob is the selected line */
        for (guard = 0; guard < 64; ++guard) {
            gptps_tui_render(t, frame, sizeof frame);
            if (strstr(frame, "> scheduler.reserve_after_skips")) break;
            gptps_tui_press(t, 'j');
        }
        CHECK(strstr(frame, "> scheduler.reserve_after_skips") != NULL);
        /* edit it to 7 */
        CHECK(gptps_tui_press(t, '\r') == 4);            /* begin edit (loads current value) */
        gptps_tui_press(t, 127); gptps_tui_press(t, 127); /* backspace existing */
        gptps_tui_press(t, '7');
        CHECK(gptps_tui_press(t, '\r') == 4);            /* commit */
        {
            char b[GPTPS_SETTINGS_VALUE_MAX];
            CHECK(gptps_settings_get(e, "scheduler.reserve_after_skips", b, sizeof b) == GPTPS_OK && strcmp(b, "7") == 0);
        }
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "set scheduler.reserve_after_skips = 7") != NULL);
        /* an invalid edit is rejected and changes nothing */
        gptps_tui_press(t, '\r'); gptps_tui_press(t, 127); gptps_tui_press(t, 'x');
        CHECK(gptps_tui_press(t, '\r') == 4);
        {
            char b[GPTPS_SETTINGS_VALUE_MAX];
            CHECK(gptps_settings_get(e, "scheduler.reserve_after_skips", b, sizeof b) == GPTPS_OK && strcmp(b, "7") == 0);
        }
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "rejected") != NULL);
        /* save to the configured path, then back to the dashboard */
        CHECK(gptps_tui_press(t, 'w') == 4);
        { FILE *sf = fopen("tui_settings.toml", "rb"); CHECK(sf != NULL); if (sf) fclose(sf); remove("tui_settings.toml"); }
        CHECK(gptps_tui_press(t, 's') == 4);             /* back to dashboard */
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "in-flight") != NULL);        /* dashboard again */
    }

    /* ---- friendliness: help overlay + action toast ---- */
    {
        CHECK(gptps_tui_press(t, '?') == 4);              /* open help overlay */
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "help")            != NULL);
        CHECK(strstr(frame, "submit its task") != NULL);  /* documents the keys */
        CHECK(strstr(frame, "settings editor") != NULL);
        CHECK(strstr(frame, "in-flight")       == NULL);  /* dashboard hidden behind help */
        CHECK(gptps_tui_press(t, 'x') == 4);              /* any key returns to dashboard */
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "in-flight")       != NULL);  /* back on the dashboard */
        /* a hotkey submit leaves a transient confirmation toast */
        CHECK(gptps_tui_press(t, 'e') == 1);
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "submitted Echo")  != NULL);
        CHECK(strstr(frame, "? help")          != NULL);  /* global keys now discoverable */
    }

    /* quit key */
    CHECK(gptps_tui_press(t, 'q') == -1);

    gptps_shutdown(e);     /* close AFTER shutdown */
    gptps_tui_close(t);
    (void)i;
    if (fails) { printf("%d tui check(s) FAILED\n", fails); return 1; }
    printf("all tui checks passed\n");
    return 0;
}
