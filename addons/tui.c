/*
 * tui.c - real-time terminal dashboard for GPTPS (see tui.h).
 *
 * An observer aggregates live state (cumulative counts, per-task stats, a recent-
 * events ring). gptps_tui_render() formats that state into a frame string (pure,
 * testable). gptps_tui_press() applies a key (submit a bound task / quit).
 * gptps_tui_run() is the blocking live loop and the only part that touches the
 * terminal (ANSI escapes; Windows VT enabled); it self-skips without a TTY.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "tui.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* ---- portable mutex + tty + terminal control ---- */
#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <conio.h>
typedef CRITICAL_SECTION tui_mutex;
static void mu_init(tui_mutex *m)    { InitializeCriticalSection(m); }
static void mu_lock(tui_mutex *m)    { EnterCriticalSection(m); }
static void mu_unlock(tui_mutex *m)  { LeaveCriticalSection(m); }
static void mu_destroy(tui_mutex *m) { DeleteCriticalSection(m); }
static int  fd_is_tty(FILE *f)       { return _isatty(_fileno(f)); }
#else
#  include <unistd.h>
#  include <termios.h>
#  include <sys/select.h>
#  include <sys/ioctl.h>
#  include <pthread.h>
typedef pthread_mutex_t tui_mutex;
static void mu_init(tui_mutex *m)    { pthread_mutex_init(m, NULL); }
static void mu_lock(tui_mutex *m)    { pthread_mutex_lock(m); }
static void mu_unlock(tui_mutex *m)  { pthread_mutex_unlock(m); }
static void mu_destroy(tui_mutex *m) { pthread_mutex_destroy(m); }
static int  fd_is_tty(FILE *f)       { return isatty(fileno(f)); }
#endif

#define TUI_MAX_TASKS 64

typedef struct {
    char     name[64], label[64];
    int      hotkey;
    void    *payload;
    size_t   plen;
    unsigned started, finished, failed, retried, dead;
    uint64_t lat_sum_ms, lat_max_ms;   /* queue->finish latency accumulators */
    unsigned lat_n;
} tui_task;

typedef struct { uint64_t ts; int kind; char name[64]; uint64_t handle; gptps_status status; } tui_event;

struct gptps_tui {
    gptps           *e;
    gptps_tui_config cfg;
    tui_mutex        mu;
    uint64_t         start_ms;
    unsigned         q, s, fin, fail, retr, dead, peak;
    tui_task         tasks[TUI_MAX_TASKS];
    int              ntasks;
    tui_event       *recent;
    int              rcap, rn, rhead;
    struct { gptps_handle h; uint64_t ts; } *lat;  /* handle->queued-ts ring for latency */
    int              lat_cap, lat_head;
    int              scroll;        /* recent-log scroll offset (lines back) */
    int              kpi;           /* effective gptps_tui_kpi (never DEFAULT) */
    int              mode;          /* effective gptps_tui_mode */
    int              dirty;         /* state changed since last paint (for ON_DEMAND) */
    /* Settings pane state - touched only by the run/press/render thread (not the
     * observer), so it needs no lock. */
    int              pane;          /* 0 = dashboard, 1 = settings editor */
    int              sel;           /* selected setting index */
    int              editing, editlen;
    char             editbuf[GPTPS_SETTINGS_VALUE_MAX];
    char             status[160];   /* last save/validation message + dashboard action toast */
    uint64_t         status_ms;     /* when the toast was set (it fades after a few seconds) */
    int              cols, rows;    /* terminal size: run() refreshes it; 80x24 headless default */
    int              quit, show_tasks, show_recent;
    int              raw_active;
#if !defined(_WIN32)
    struct termios   saved_termios;
#else
    DWORD            saved_out_mode; int modes_saved;
#endif
};

/* ---- state (under mu) ---- */
static tui_task *task_for(gptps_tui *t, const char *name)
{
    int i;
    for (i = 0; i < t->ntasks; ++i) if (strcmp(t->tasks[i].name, name) == 0) return &t->tasks[i];
    if (t->ntasks < TUI_MAX_TASKS) {
        tui_task *tk = &t->tasks[t->ntasks++];
        memset(tk, 0, sizeof *tk);
        strncpy(tk->name, name, sizeof tk->name - 1);
        strncpy(tk->label, name, sizeof tk->label - 1);
        return tk;
    }
    return NULL;
}

static void tui_on_event(const gptps_event *ev, void *ud)
{
    gptps_tui *t = (gptps_tui *)ud;
    tui_task *tk;
    unsigned inflight;
    mu_lock(&t->mu);
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:        t->q++;    break;
        case GPTPS_EV_STARTED:       t->s++;    break;
        case GPTPS_EV_FINISHED:      t->fin++;  break;
        case GPTPS_EV_FAILED:        t->fail++; break;
        case GPTPS_EV_RETRIED:       t->retr++; break;
        case GPTPS_EV_DEAD_LETTERED: t->dead++; break;
        default: break;
    }
    inflight = t->s - t->fin - t->fail;          /* per-attempt: never underflows */
    if (inflight > t->peak) t->peak = inflight;
    t->dirty = 1;                                /* something changed (for ON_DEMAND repaint) */

    /* MINIMAL stops here - counts only, ~no per-event cost. NORMAL+ does the
     * per-task table + recent log; latency tracking runs only when its ring exists
     * (FULL), and that ring is freed when the KPI level is lowered. */
    if (t->kpi < GPTPS_TUI_KPI_NORMAL) { mu_unlock(&t->mu); return; }

    if (t->lat && ev->kind == GPTPS_EV_QUEUED) {          /* remember queued time per handle */
        t->lat[t->lat_head].h = ev->handle; t->lat[t->lat_head].ts = ev->ts_ms;
        t->lat_head = (t->lat_head + 1) % t->lat_cap;
    }

    if ((tk = task_for(t, ev->task_name)) != NULL) {
        switch (ev->kind) {
            case GPTPS_EV_STARTED:       tk->started++;  break;
            case GPTPS_EV_FINISHED:
                tk->finished++;
                if (t->lat) {                             /* FULL: resolve this handle's latency */
                    int j;
                    for (j = 0; j < t->lat_cap; ++j)
                        if (t->lat[j].h == ev->handle) {
                            uint64_t l = (ev->ts_ms >= t->lat[j].ts) ? ev->ts_ms - t->lat[j].ts : 0;
                            tk->lat_sum_ms += l; tk->lat_n++;
                            if (l > tk->lat_max_ms) tk->lat_max_ms = l;
                            t->lat[j].h = 0;              /* consume */
                            break;
                        }
                }
                break;
            case GPTPS_EV_FAILED:        tk->failed++;   break;
            case GPTPS_EV_RETRIED:       tk->retried++;  break;
            case GPTPS_EV_DEAD_LETTERED: tk->dead++;     break;
            default: break;
        }
    }
    if (t->rcap > 0) {
        tui_event *re = &t->recent[t->rhead];
        re->ts = ev->ts_ms; re->kind = ev->kind; re->handle = ev->handle; re->status = ev->status;
        strncpy(re->name, ev->task_name, sizeof re->name - 1); re->name[sizeof re->name - 1] = 0;
        t->rhead = (t->rhead + 1) % t->rcap;
        if (t->rn < t->rcap) t->rn++;
    }
    mu_unlock(&t->mu);
}

static const char *kpi_str(int k)
{ return k == GPTPS_TUI_KPI_MINIMAL ? "minimal" : k == GPTPS_TUI_KPI_NORMAL ? "normal" : "full"; }
static const char *mode_str(int m)
{ return m == GPTPS_TUI_CONTINUOUS ? "realtime" : m == GPTPS_TUI_ON_DEMAND ? "on-demand" : "paused"; }

static const char *kind_str(int k)
{
    switch (k) {
        case GPTPS_EV_QUEUED:        return "QUEUED";
        case GPTPS_EV_STARTED:       return "STARTED";
        case GPTPS_EV_FINISHED:      return "FINISHED";
        case GPTPS_EV_FAILED:        return "FAILED";
        case GPTPS_EV_RETRIED:       return "RETRIED";
        case GPTPS_EV_DEAD_LETTERED: return "DEAD";
        default:                     return "?";
    }
}

/* bounded formatted append into buf at pos; returns new pos (clamped to cap) */
static size_t appendf(char *buf, size_t cap, size_t pos, const char *fmt, ...)
{
    va_list ap; int n;
    if (pos >= cap) return pos;
    va_start(ap, fmt);
    n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    pos += (size_t)n;
    return pos > cap ? cap : pos;
}

/* Settings pane (no t->mu: reads only run-thread-local pane state + the registry,
 * whose get_info takes its own locks - holding t->mu here would deadlock the tui
 * read_fns). */
static size_t render_settings(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0, i, n;
    int color = t->cfg.color;
    const char *B = color ? "\x1b[1m" : "", *INV = color ? "\x1b[7m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    n = gptps_settings_count(t->e);
    if (t->sel < 0) t->sel = 0;
    if (n && (size_t)t->sel >= n) t->sel = (int)n - 1;
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 %s settings%s  (%lu)\n\n", B, t->cfg.title, X, (unsigned long)n);
    for (i = 0; i < n; ++i) {
        gptps_setting_info info;
        memset(&info, 0, sizeof info); info.struct_size = sizeof info;
        if (gptps_settings_get_info(t->e, i, &info) != GPTPS_OK) continue;
        pos = appendf(buf, cap, pos, "%s%s%-32s = %-18s %s%s\n",
                      (i == (size_t)t->sel) ? INV : "", (i == (size_t)t->sel) ? "> " : "  ",
                      info.key, info.value, info.hot ? "" : "(restart)", X);
    }
    if (t->editing) pos = appendf(buf, cap, pos, "\n%sedit:%s %s_\n", B, X, t->editbuf);
    if (t->status[0]) pos = appendf(buf, cap, pos, "%s%s%s\n", D, t->status, X);
    pos = appendf(buf, cap, pos, "\n%skeys:%s j/k move  enter edit  w save  s/esc back  q quit\n", D, X);
    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

/* set a transient action message ("toast") shown briefly on the dashboard */
static void toast(gptps_tui *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->status, sizeof t->status, fmt, ap);
    va_end(ap);
    t->status_ms = gptps_now_ms(NULL);
}

/* terminal width clamped to a sane drawing range */
static int draw_width(const gptps_tui *t) { int W = t->cols > 0 ? t->cols : 80; if (W < 24) W = 24; if (W > 200) W = 200; return W; }

/* full-width inverse title bar (color mode): left title, right-aligned stats */
static size_t titlebar(char *buf, size_t cap, size_t pos, gptps_tui *t, int W, double up, double rate)
{
    char line[256], stats[64];
    int len, sl;
    if (W > (int)sizeof line - 1) W = (int)sizeof line - 1;
    len = snprintf(line, sizeof line, " GPTPS  %s", t->cfg.title ? t->cfg.title : "tasks");
    if (len < 0) len = 0;
    sl = snprintf(stats, sizeof stats, "up %.1fs  %.1f done/s ", up, rate);
    if (sl > 0 && len + 2 + sl <= W) { while (len < W - sl) line[len++] = ' '; memcpy(line + len, stats, (size_t)sl); len += sl; }
    if (len > W) len = W;
    while (len < W) line[len++] = ' ';
    line[len] = 0;
    return appendf(buf, cap, pos, "\x1b[7m\x1b[1m%s\x1b[0m\n", line);
}

/* horizontal rule across the width (color mode) */
static size_t rule(char *buf, size_t cap, size_t pos, int W, int uni)
{
    int i;
    pos = appendf(buf, cap, pos, "\x1b[2m");
    for (i = 0; i < W; ++i) pos = appendf(buf, cap, pos, "%s", uni ? "\xe2\x94\x80" : "-");
    return appendf(buf, cap, pos, "\x1b[0m\n");
}

/* Help overlay: every key + what it does. Pure; reads only cfg. */
static size_t render_help(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0;
    int color = t->cfg.color;
    const char *B = color ? "\x1b[1m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    const char *title = t->cfg.title ? t->cfg.title : "tasks";
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 %s \xc2\xb7 help%s\n\n", B, title, X);
    pos = appendf(buf, cap, pos, "%sDashboard%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s submit its task\n", "<hotkey>");
    pos = appendf(buf, cap, pos, "  %-9s scroll the event log (older / newer)\n", "k / j");
    pos = appendf(buf, cap, pos, "  %-9s open the settings editor\n", "s");
    pos = appendf(buf, cap, pos, "  %-9s cycle KPI detail (minimal/normal/full)\n", "m");
    pos = appendf(buf, cap, pos, "  %-9s pause / resume live updates\n", "p");
    pos = appendf(buf, cap, pos, "  %-9s toggle this help\n", "?");
    pos = appendf(buf, cap, pos, "  %-9s quit\n\n", "q / Esc");
    pos = appendf(buf, cap, pos, "%sSettings editor%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s move selection\n", "k / j");
    pos = appendf(buf, cap, pos, "  %-9s edit value (Enter commit, Esc cancel)\n", "Enter");
    pos = appendf(buf, cap, pos, "  %-9s save settings to file\n", "w");
    pos = appendf(buf, cap, pos, "  %-9s back to dashboard\n", "s / Esc");
    pos = appendf(buf, cap, pos, "\n%spress any key to return%s\n", D, X);
    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0;
    int i, color, uni, W;
    const char *B, *D, *R, *G, *X;
    double up, rate;
    if (!t || !buf || cap == 0) { if (buf && cap) buf[0] = 0; return 0; }
    if (t->pane == 1) return render_settings(t, buf, cap);
    if (t->pane == 2) return render_help(t, buf, cap);
    color = t->cfg.color; uni = t->cfg.unicode; W = draw_width(t);
    B = color ? "\x1b[1m" : ""; D = color ? "\x1b[2m" : ""; R = color ? "\x1b[31m" : "";
    G = color ? "\x1b[32m" : ""; X = color ? "\x1b[0m" : "";

    mu_lock(&t->mu);
    up = (double)(gptps_now_ms(NULL) - t->start_ms) / 1000.0;
    rate = (up > 0.05) ? (double)t->fin / up : 0.0;

    /* header: a framed title bar when color is on; a plain line otherwise */
    if (color) { pos = titlebar(buf, cap, pos, t, W, up, rate); pos = rule(buf, cap, pos, W, uni); }
    else pos = appendf(buf, cap, pos, "GPTPS \xc2\xb7 %s   up %.1fs   %.1f done/s\n", t->cfg.title, up, rate);

    {   /* in-flight gauge, scaled to the terminal width */
        unsigned inflt = t->s - t->fin - t->fail, j, gw, w;
        const char *fillc = uni ? "\xe2\x96\x88" : "#";   /* full block / hash */
        const char *trakc = uni ? "\xe2\x96\x91" : ".";   /* light shade / dot  */
        const char *C = color ? "\x1b[36m" : "";          /* cyan bar */
        gw = (unsigned)(W / 3); if (gw < 10) gw = 10; if (gw > 40) gw = 40;
        w = (t->peak && inflt) ? (inflt * gw / t->peak) : 0; if (w > gw) w = gw;
        pos = appendf(buf, cap, pos, "queued %u  started %u  in-flight %u  %s[%s", t->q, t->s, inflt, D, C);
        for (j = 0; j < gw; ++j) pos = appendf(buf, cap, pos, "%s", j < w ? fillc : trakc);
        pos = appendf(buf, cap, pos, "%s%s]%s peak %u\n", X, D, X, t->peak);
    }
    pos = appendf(buf, cap, pos, "%sfinished %u%s  %sfailed %u%s  retried %u  %sdead %u%s\n",
                  G, t->fin, X, (t->fail ? R : ""), t->fail, X, t->retr, (t->dead ? R : ""), t->dead, X);
    pos = appendf(buf, cap, pos, "%skpi:%s mode:%s refresh:%ums%s\n",
                  D, kpi_str(t->kpi), mode_str(t->mode), t->cfg.refresh_ms, X);

    if (t->show_tasks && t->kpi >= GPTPS_TUI_KPI_NORMAL) {
        pos = appendf(buf, cap, pos, "\n%sTASKS%s\n", B, X);
        pos = appendf(buf, cap, pos, "%s  %-16s %5s %5s %5s %5s %5s %8s  key%s\n",
                      D, "label", "run", "ok", "fail", "dead", "ok%", "avg ms", X);
        for (i = 0; i < t->ntasks; ++i) {
            tui_task *k = &t->tasks[i];
            char key[8], pct[8], lat[12];
            unsigned terminal = k->finished + k->dead;
            const char *pc = "";
            if (k->hotkey) snprintf(key, sizeof key, "[%c]", k->hotkey); else key[0] = 0;
            if (terminal) {
                unsigned okp = k->finished * 100u / terminal;
                snprintf(pct, sizeof pct, "%3u%%", okp);
                if (color) pc = (okp >= 90) ? G : (okp >= 50) ? "\x1b[33m" : R;   /* green/yellow/red */
            } else strcpy(pct, "  --");
            if (k->lat_n) snprintf(lat, sizeof lat, "%8.1f", (double)k->lat_sum_ms / k->lat_n); else strcpy(lat, "      --");
            pos = appendf(buf, cap, pos, "  %-16.16s %5u %5u %5u %5u %s%5s%s %8s  %s\n",
                          k->label, k->started, k->finished, k->failed, k->dead, pc, pct, color ? X : "", lat, key);
        }
    }

    if (t->show_recent && t->rcap > 0 && t->kpi >= GPTPS_TUI_KPI_NORMAL) {
        int shown = t->rn, start, maxoff, used = 0, avail;
        size_t z;
        if (shown > t->cfg.max_recent) shown = t->cfg.max_recent;
        for (z = 0; z < pos; ++z) if (buf[z] == '\n') ++used;     /* vertical fit: stay within rows */
        avail = (t->rows > 0 ? t->rows : 24) - used - 3;
        if (avail < 1) avail = 1;
        if (shown > avail) shown = avail;
        maxoff = t->rn - shown; if (maxoff < 0) maxoff = 0;
        if (t->scroll > maxoff) t->scroll = maxoff;     /* clamp (keys can over-scroll) */
        if (t->scroll < 0) t->scroll = 0;
        start = (t->rhead - t->scroll - shown + t->rcap * 4) % t->rcap; /* window end = newest - scroll */
        if (t->scroll > 0) pos = appendf(buf, cap, pos, "\n%sRECENT%s  %s(scrolled +%d)%s\n", B, X, D, t->scroll, X);
        else               pos = appendf(buf, cap, pos, "\n%sRECENT%s\n", B, X);
        for (i = 0; i < shown; ++i) {
            tui_event *re = &t->recent[(start + i) % t->rcap];
            const char *kc = (re->kind == GPTPS_EV_FAILED || re->kind == GPTPS_EV_DEAD_LETTERED) ? R
                           : (re->kind == GPTPS_EV_FINISHED) ? G : D;
            double rel = (double)(re->ts - t->start_ms) / 1000.0; /* seconds since install */
            pos = appendf(buf, cap, pos, "  %s%6.1f%s %s%-8s%s %-14.14s #%llu\n",
                          D, rel, X, kc, kind_str(re->kind), X, re->name, (unsigned long long)re->handle);
        }
    }

    /* transient action toast (fades after a few seconds) */
    if (t->status[0] && t->status_ms && (gptps_now_ms(NULL) - t->status_ms) < 3000u)
        pos = appendf(buf, cap, pos, "\n%s%s%s\n", (color ? "\x1b[36m" : ""), t->status, X);

    /* key legend: task hotkeys first, then the global keys */
    pos = appendf(buf, cap, pos, "\n%skeys:%s ", D, X);
    for (i = 0; i < t->ntasks; ++i)
        if (t->tasks[i].hotkey)
            pos = appendf(buf, cap, pos, "[%c] %s  ", t->tasks[i].hotkey, t->tasks[i].label);
    pos = appendf(buf, cap, pos, "%s%s? help  s settings  m kpi  p pause  k/j scroll  q quit%s\n",
                  (t->ntasks ? " \xc2\xb7  " : ""), D, X);
    mu_unlock(&t->mu);

    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

/* Settings-pane key handling. Returns 4 (settings interaction) or -1 (quit). */
static int settings_press(gptps_tui *t, int key)
{
    gptps_setting_info info;
    if (t->editing) {
        if (key == 27) { t->editing = 0; t->editbuf[0] = 0; t->editlen = 0; return 4; }   /* cancel */
        if (key == '\r' || key == '\n') {                                                 /* commit */
            memset(&info, 0, sizeof info); info.struct_size = sizeof info;
            if (gptps_settings_get_info(t->e, (size_t)t->sel, &info) == GPTPS_OK) {
                gptps_status st = gptps_settings_set(t->e, info.key, t->editbuf);
                if (st == GPTPS_OK) snprintf(t->status, sizeof t->status, "set %.70s = %.70s", info.key, t->editbuf);
                else                snprintf(t->status, sizeof t->status, "rejected: %s", gptps_strerror(st));
            }
            t->editing = 0; t->editbuf[0] = 0; t->editlen = 0;
            return 4;
        }
        if (key == 8 || key == 127) { if (t->editlen > 0) t->editbuf[--t->editlen] = 0; return 4; } /* backspace */
        if (key >= 32 && key < 127 && t->editlen < (int)sizeof t->editbuf - 1) { t->editbuf[t->editlen++] = (char)key; t->editbuf[t->editlen] = 0; }
        return 4;
    }
    switch (key) {
        case 'q': case 'Q': mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu); return -1;
        case 27: case 's': case 'S': case '\t': t->pane = 0; t->status[0] = 0; return 4;   /* back to dashboard */
        case 'j': case 'J': { size_t n = gptps_settings_count(t->e); if (n && (size_t)t->sel + 1 < n) t->sel++; return 4; }
        case 'k': case 'K': if (t->sel > 0) t->sel--; return 4;
        case '\r': case '\n':                                                              /* start editing */
            memset(&info, 0, sizeof info); info.struct_size = sizeof info;
            if (gptps_settings_get_info(t->e, (size_t)t->sel, &info) == GPTPS_OK) {
                snprintf(t->editbuf, sizeof t->editbuf, "%s", info.value);
                t->editlen = (int)strlen(t->editbuf); t->editing = 1;
            }
            return 4;
        case 'w': case 'W': {
            gptps_status st = gptps_settings_save(t->e, t->cfg.settings_path);
            snprintf(t->status, sizeof t->status, st == GPTPS_OK ? "saved" : "save failed: %s", gptps_strerror(st));
            return 4;
        }
        default: return 4;
    }
}

int gptps_tui_press(gptps_tui *t, int key)
{
    const char *name = NULL, *label = NULL;
    const void *pl = NULL;
    size_t pn = 0;
    int i;
    if (!t) return 0;
    if (t->pane == 1) return settings_press(t, key);   /* settings editor handles its own keys */
    if (t->pane == 2) {                                 /* help overlay: any key returns */
        if (key == 'q' || key == 'Q') { mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu); return -1; }
        t->pane = 0; return 4;
    }
    if (key == 'q' || key == 'Q' || key == 27) {
        mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu);
        return -1;
    }
    if (key == '?') { t->pane = 2; return 4; }      /* open the help overlay */
    mu_lock(&t->mu);
    for (i = 0; i < t->ntasks; ++i)
        if (t->tasks[i].hotkey == key) { name = t->tasks[i].name; label = t->tasks[i].label; pl = t->tasks[i].payload; pn = t->tasks[i].plen; break; }
    mu_unlock(&t->mu);                 /* submit OUTSIDE the lock: QUEUED fires our observer */
    if (name) { gptps_handle h; gptps_submit(t->e, name, pl, pn, &h); toast(t, "submitted %s", label ? label : name); return 1; }
    if (key == 'm' || key == 'M') {                 /* cycle KPI level (configure cost live) */
        int nk; mu_lock(&t->mu); nk = (t->kpi >= GPTPS_TUI_KPI_FULL) ? GPTPS_TUI_KPI_MINIMAL : t->kpi + 1; mu_unlock(&t->mu);
        gptps_tui_set_kpi(t, (gptps_tui_kpi)nk); toast(t, "kpi -> %s", kpi_str(nk)); return 3;
    }
    if (key == 'p' || key == 'P') {                 /* toggle pause */
        int nm; mu_lock(&t->mu); nm = (t->mode == GPTPS_TUI_PAUSED) ? GPTPS_TUI_CONTINUOUS : GPTPS_TUI_PAUSED; mu_unlock(&t->mu);
        gptps_tui_set_mode(t, (gptps_tui_mode)nm); toast(t, nm == GPTPS_TUI_PAUSED ? "paused" : "resumed"); return 3;
    }
    if (key == 'k' || key == 'K') { mu_lock(&t->mu); t->scroll += 1; mu_unlock(&t->mu); return 2; } /* scroll to older */
    if (key == 'j' || key == 'J') { mu_lock(&t->mu); if (t->scroll > 0) t->scroll -= 1; mu_unlock(&t->mu); return 2; } /* newer */
    if (key == 's' || key == 'S') { t->pane = 1; t->status[0] = 0; return 4; }   /* open the settings pane */
    return 0;
}

gptps_status gptps_tui_add_task(gptps_tui *t, const char *task_name, const char *label,
                                int hotkey, const void *payload, size_t len)
{
    tui_task *tk;
    void *cp = NULL;
    if (!t || !task_name) return GPTPS_E_INVAL;
    if (len) { cp = malloc(len); if (!cp) return GPTPS_E_NOMEM; memcpy(cp, payload, len); }
    mu_lock(&t->mu);
    tk = task_for(t, task_name);
    if (tk) {
        if (label) { strncpy(tk->label, label, sizeof tk->label - 1); tk->label[sizeof tk->label - 1] = 0; }
        tk->hotkey = hotkey;
        free(tk->payload); tk->payload = cp; tk->plen = len; cp = NULL;
    }
    mu_unlock(&t->mu);
    if (cp) free(cp);
    return tk ? GPTPS_OK : GPTPS_E_FULL;
}

/* ---- terminal control (live loop only) ---- */
#if !defined(_WIN32)
static void term_enable(gptps_tui *t)
{
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &t->saved_termios) != 0) return;
    raw = t->saved_termios;
    raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO));
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) t->raw_active = 1;
}
static void term_restore(gptps_tui *t)
{ if (t->raw_active) { tcsetattr(STDIN_FILENO, TCSANOW, &t->saved_termios); t->raw_active = 0; } }
static int term_poll_key(int ms)
{
    fd_set fds; struct timeval tv; unsigned char c;
    FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0 && read(STDIN_FILENO, &c, 1) == 1) return c;
    return -1;
}
static void term_size(gptps_tui *t)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        t->cols = ws.ws_col;
        if (ws.ws_row > 0) t->rows = ws.ws_row;
    }
}
#else
static void term_enable(gptps_tui *t)
{
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE); DWORD m;
    if (GetConsoleMode(ho, &m)) { t->saved_out_mode = m; t->modes_saved = 1;
        SetConsoleMode(ho, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
    t->raw_active = 1;
}
static void term_restore(gptps_tui *t)
{ if (t->modes_saved) { SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), t->saved_out_mode); t->modes_saved = 0; } t->raw_active = 0; }
static int term_poll_key(int ms)
{
    int waited = 0;
    while (waited <= ms) { if (_kbhit()) return _getch(); Sleep(15); waited += 15; }
    return -1;
}
static void term_size(gptps_tui *t)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci)) {
        int c = ci.srWindow.Right - ci.srWindow.Left + 1, r = ci.srWindow.Bottom - ci.srWindow.Top + 1;
        if (c > 0) t->cols = c;
        if (r > 0) t->rows = r;
    }
}
#endif

/* write a frame flicker-free: erase each line's tail (ESC[K) instead of clearing
 * the whole screen, so unchanged regions don't flash. */
static void write_frame(FILE *out, const char *s)
{
    const char *p = s, *nl;
    while ((nl = strchr(p, '\n')) != NULL) {
        fwrite(p, 1, (size_t)(nl - p), out);
        fputs("\x1b[K\n", out);
        p = nl + 1;
    }
    if (*p) { fputs(p, out); fputs("\x1b[K", out); }
}

void gptps_tui_run(gptps_tui *t)
{
    char *buf; size_t cap = 16384;
    int first = 1, kp = 0;
    if (!t || !t->cfg.interactive || !fd_is_tty(t->cfg.out)) return; /* needs a terminal */
    buf = (char *)malloc(cap);
    if (!buf) return;
    term_enable(t);
    fputs("\x1b[?25l", t->cfg.out);                 /* hide cursor */
    for (;;) {
        int q, md, key, paint;
        uint32_t rms;
        mu_lock(&t->mu); q = t->quit; md = t->mode; rms = t->cfg.refresh_ms; mu_unlock(&t->mu);
        if (q) break;
        /* cadence: CONTINUOUS every frame; ON_DEMAND when state changed or a key
         * was pressed; PAUSED only after a key (or the first frame) */
        paint = first || kp || (md == GPTPS_TUI_CONTINUOUS);
        if (!paint && md == GPTPS_TUI_ON_DEMAND) { mu_lock(&t->mu); paint = t->dirty; mu_unlock(&t->mu); }
        if (paint) {
            mu_lock(&t->mu); t->dirty = 0; mu_unlock(&t->mu);
            term_size(t);                            /* adapt the layout to the current window */
            gptps_tui_render(t, buf, cap);
            if (first) fputs("\x1b[2J", t->cfg.out); /* clear once; then redraw in place */
            fputs("\x1b[H", t->cfg.out);             /* home */
            write_frame(t->cfg.out, buf);            /* per-line erase => no full-screen flash */
            fputs("\x1b[J", t->cfg.out);             /* wipe any lines a shorter frame left behind */
            fflush(t->cfg.out);
        }
        first = 0; kp = 0;
        key = term_poll_key((int)(rms ? rms : 250));
        if (key >= 0) { gptps_tui_press(t, key); kp = 1; }
    }
    fputs("\x1b[?25h", t->cfg.out); fflush(t->cfg.out); /* show cursor */
    term_restore(t);
    free(buf);
}

/* ---- settings registry bindings (target = the gptps_tui) ---- */
static const char *const TUI_KPI_CHOICES[]  = { "minimal", "normal", "full", 0 };
static const char *const TUI_MODE_CHOICES[] = { "realtime", "on-demand", "paused", 0 };

static size_t ts_rd_refresh(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; unsigned ms; mu_lock(&t->mu); ms = t->cfg.refresh_ms; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%u", ms); }
static gptps_status ts_wr_refresh(void *p, const char *v) { return gptps_tui_set_refresh((gptps_tui *)p, (uint32_t)strtoul(v, NULL, 10)); }
static size_t ts_rd_kpi(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int k; mu_lock(&t->mu); k = t->kpi; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%s", kpi_str(k)); }
static gptps_status ts_wr_kpi(void *p, const char *v) { int k = (strcmp(v, "minimal") == 0) ? GPTPS_TUI_KPI_MINIMAL : (strcmp(v, "normal") == 0) ? GPTPS_TUI_KPI_NORMAL : GPTPS_TUI_KPI_FULL; return gptps_tui_set_kpi((gptps_tui *)p, (gptps_tui_kpi)k); }
static size_t ts_rd_mode(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int m; mu_lock(&t->mu); m = t->mode; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%s", mode_str(m)); }
static gptps_status ts_wr_mode(void *p, const char *v) { int m = (strcmp(v, "realtime") == 0) ? GPTPS_TUI_CONTINUOUS : (strcmp(v, "on-demand") == 0) ? GPTPS_TUI_ON_DEMAND : GPTPS_TUI_PAUSED; return gptps_tui_set_mode((gptps_tui *)p, (gptps_tui_mode)m); }

static void tui_register_settings(gptps *e, gptps_tui *t)
{
    gptps_setting_def d;
    memset(&d, 0, sizeof d); d.struct_size = sizeof d; d.target = t; d.hot = 1;
    d.key = "tui.refresh_ms"; d.type = GPTPS_SETTING_UINT; d.desc = "dashboard redraw interval (ms)"; d.choices = NULL; d.read = ts_rd_refresh; d.write = ts_wr_refresh;
    gptps_register_setting(e, &d);
    d.key = "tui.kpi"; d.type = GPTPS_SETTING_ENUM; d.desc = "KPI detail level"; d.choices = TUI_KPI_CHOICES; d.read = ts_rd_kpi; d.write = ts_wr_kpi;
    gptps_register_setting(e, &d);
    d.key = "tui.mode"; d.type = GPTPS_SETTING_ENUM; d.desc = "redraw cadence"; d.choices = TUI_MODE_CHOICES; d.read = ts_rd_mode; d.write = ts_wr_mode;
    gptps_register_setting(e, &d);
}

gptps_tui *gptps_tui_install(gptps *e, const gptps_tui_config *cfg)
{
    gptps_tui *t;
    int max;
    if (!e) return NULL;
    t = (gptps_tui *)calloc(1, sizeof *t);
    if (!t) return NULL;
    mu_init(&t->mu);
    t->e = e;
    if (cfg) t->cfg = *cfg;
    if (!t->cfg.out) t->cfg.out = stdout;
    if (t->cfg.refresh_ms == 0) t->cfg.refresh_ms = 250;
    if (t->cfg.color < 0)       t->cfg.color = fd_is_tty(t->cfg.out);
    if (t->cfg.interactive < 0) t->cfg.interactive = fd_is_tty(t->cfg.out);
    if (t->cfg.unicode < 0)     t->cfg.unicode = t->cfg.color;   /* auto: follow color */
    t->cols = 80; t->rows = 24;   /* headless defaults; the live loop refreshes from the terminal */
    t->show_tasks  = (t->cfg.show_tasks  >= 0);   /* default on; <0 hides */
    t->show_recent = (t->cfg.show_recent >= 0);
    max = (t->cfg.max_recent > 0) ? t->cfg.max_recent : 8;
    if (max > 64) max = 64;
    t->cfg.max_recent = max;
    t->rcap = max;
    t->recent = (tui_event *)calloc((size_t)max, sizeof(tui_event));
    if (!t->recent) { mu_destroy(&t->mu); free(t); return NULL; }
    t->kpi = (t->cfg.kpi == GPTPS_TUI_KPI_DEFAULT) ? GPTPS_TUI_KPI_FULL : t->cfg.kpi;
    if (t->kpi < GPTPS_TUI_KPI_MINIMAL) t->kpi = GPTPS_TUI_KPI_MINIMAL;
    if (t->kpi > GPTPS_TUI_KPI_FULL)    t->kpi = GPTPS_TUI_KPI_FULL;
    t->mode = t->cfg.mode;
    if (t->mode < GPTPS_TUI_CONTINUOUS || t->mode > GPTPS_TUI_PAUSED) t->mode = GPTPS_TUI_CONTINUOUS;
    t->lat_cap = (t->cfg.latency_window > 0) ? t->cfg.latency_window : 1024;
    if (t->lat_cap > (1 << 20)) t->lat_cap = (1 << 20);
    t->lat = NULL;         /* the latency ring exists only at KPI FULL (else its RAM is reclaimed) */
    if (t->kpi == GPTPS_TUI_KPI_FULL)
        t->lat = calloc((size_t)t->lat_cap, sizeof *t->lat);
    t->start_ms = gptps_now_ms(NULL);
    if (gptps_register_observer(e, tui_on_event, t) != GPTPS_OK) {
        free(t->recent); mu_destroy(&t->mu); free(t); return NULL;
    }
    tui_register_settings(e, t);   /* expose tui.refresh_ms / tui.kpi / tui.mode */
    return t;
}

void gptps_tui_close(gptps_tui *t)
{
    int i;
    if (!t) return;
    term_restore(t);                 /* in case run() was interrupted */
    for (i = 0; i < t->ntasks; ++i) free(t->tasks[i].payload);
    free(t->recent);
    free(t->lat);
    mu_destroy(&t->mu);
    free(t);
}

/* ---- runtime reconfiguration ---- */
/* caller holds mu: make the latency ring match the current KPI level */
static void lat_apply(gptps_tui *t)
{
    if (t->kpi == GPTPS_TUI_KPI_FULL) {
        if (!t->lat) { t->lat = calloc((size_t)t->lat_cap, sizeof *t->lat); t->lat_head = 0; }
    } else if (t->lat) {
        free(t->lat); t->lat = NULL; t->lat_head = 0;   /* reclaim the RAM */
    }
}

gptps_status gptps_tui_set_kpi(gptps_tui *t, gptps_tui_kpi level)
{
    if (!t) return GPTPS_E_INVAL;
    if (level == GPTPS_TUI_KPI_DEFAULT) level = GPTPS_TUI_KPI_FULL;
    if (level < GPTPS_TUI_KPI_MINIMAL || level > GPTPS_TUI_KPI_FULL) return GPTPS_E_INVAL;
    mu_lock(&t->mu);
    t->kpi = level;
    lat_apply(t);            /* allocate at FULL, free below FULL */
    t->dirty = 1;
    mu_unlock(&t->mu);
    return GPTPS_OK;
}

gptps_status gptps_tui_set_mode(gptps_tui *t, gptps_tui_mode mode)
{
    if (!t) return GPTPS_E_INVAL;
    if (mode < GPTPS_TUI_CONTINUOUS || mode > GPTPS_TUI_PAUSED) return GPTPS_E_INVAL;
    mu_lock(&t->mu); t->mode = mode; t->dirty = 1; mu_unlock(&t->mu);
    return GPTPS_OK;
}

gptps_status gptps_tui_set_refresh(gptps_tui *t, uint32_t refresh_ms)
{
    if (!t) return GPTPS_E_INVAL;
    mu_lock(&t->mu); t->cfg.refresh_ms = refresh_ms ? refresh_ms : 250; mu_unlock(&t->mu);
    return GPTPS_OK;
}

void gptps_tui_snapshot(gptps_tui *t)
{
    char *buf; size_t cap = 16384;
    if (!t) return;
    buf = (char *)malloc(cap);
    if (!buf) return;
    gptps_tui_render(t, buf, cap);     /* one frame, right now, regardless of mode */
    fputs(buf, t->cfg.out); fputc('\n', t->cfg.out); fflush(t->cfg.out);
    free(buf);
}
