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

size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0;
    int i, color;
    const char *B, *D, *R, *G, *X;
    double up;
    if (!t || !buf || cap == 0) { if (buf && cap) buf[0] = 0; return 0; }
    color = t->cfg.color;
    B = color ? "\x1b[1m" : ""; D = color ? "\x1b[2m" : ""; R = color ? "\x1b[31m" : "";
    G = color ? "\x1b[32m" : ""; X = color ? "\x1b[0m" : "";

    mu_lock(&t->mu);
    up = (double)(gptps_now_ms(NULL) - t->start_ms) / 1000.0;
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 %s%s   up %.1fs   %.1f done/s\n",
                  B, t->cfg.title, X, up, (up > 0.05 ? (double)t->fin / up : 0.0));
    {   /* in-flight gauge bar (ASCII; scaled to peak) */
        unsigned inflt = t->s - t->fin - t->fail, w, j;
        char bar[21];
        w = (t->peak && inflt) ? (inflt * 20u / t->peak) : 0; if (w > 20u) w = 20u;
        for (j = 0; j < 20u; ++j) bar[j] = (j < w) ? '#' : ' ';
        bar[20] = 0;
        pos = appendf(buf, cap, pos, "queued %u  started %u  in-flight %u [%s] peak %u\n",
                      t->q, t->s, inflt, bar, t->peak);
    }
    pos = appendf(buf, cap, pos, "%sfinished %u%s  %sfailed %u%s  retried %u  %sdead %u%s\n",
                  G, t->fin, X, (t->fail ? R : ""), t->fail, X, t->retr, (t->dead ? R : ""), t->dead, X);
    pos = appendf(buf, cap, pos, "%skpi:%s mode:%s refresh:%ums%s\n",
                  D, kpi_str(t->kpi), mode_str(t->mode), t->cfg.refresh_ms, X);

    if (t->show_tasks && t->kpi >= GPTPS_TUI_KPI_NORMAL) {
        pos = appendf(buf, cap, pos, "\n%sTASKS%s\n", B, X);
        pos = appendf(buf, cap, pos, "  %-16s %5s %5s %5s %5s %5s %8s  key\n",
                      "label", "run", "ok", "fail", "dead", "ok%", "avg ms");
        for (i = 0; i < t->ntasks; ++i) {
            tui_task *k = &t->tasks[i];
            char key[8], pct[8], lat[12];
            unsigned terminal = k->finished + k->dead;
            if (k->hotkey) snprintf(key, sizeof key, "[%c]", k->hotkey); else key[0] = 0;
            if (terminal) snprintf(pct, sizeof pct, "%3u%%", k->finished * 100u / terminal); else strcpy(pct, "  --");
            if (k->lat_n) snprintf(lat, sizeof lat, "%8.1f", (double)k->lat_sum_ms / k->lat_n); else strcpy(lat, "      --");
            pos = appendf(buf, cap, pos, "  %-16.16s %5u %5u %5u %5u %5s %8s  %s\n",
                          k->label, k->started, k->finished, k->failed, k->dead, pct, lat, key);
        }
    }

    if (t->show_recent && t->rcap > 0 && t->kpi >= GPTPS_TUI_KPI_NORMAL) {
        int shown = t->rn, start, maxoff;
        if (shown > t->cfg.max_recent) shown = t->cfg.max_recent;
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

    /* hotkey legend */
    pos = appendf(buf, cap, pos, "\n%skeys:%s ", D, X);
    for (i = 0; i < t->ntasks; ++i)
        if (t->tasks[i].hotkey)
            pos = appendf(buf, cap, pos, "[%c] %s  ", t->tasks[i].hotkey, t->tasks[i].label);
    pos = appendf(buf, cap, pos, "[q] quit\n");
    mu_unlock(&t->mu);

    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

int gptps_tui_press(gptps_tui *t, int key)
{
    const char *name = NULL;
    const void *pl = NULL;
    size_t pn = 0;
    int i;
    if (!t) return 0;
    if (key == 'q' || key == 'Q' || key == 27) {
        mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu);
        return -1;
    }
    mu_lock(&t->mu);
    for (i = 0; i < t->ntasks; ++i)
        if (t->tasks[i].hotkey == key) { name = t->tasks[i].name; pl = t->tasks[i].payload; pn = t->tasks[i].plen; break; }
    mu_unlock(&t->mu);                 /* submit OUTSIDE the lock: QUEUED fires our observer */
    if (name) { gptps_handle h; gptps_submit(t->e, name, pl, pn, &h); return 1; }
    if (key == 'm' || key == 'M') {                 /* cycle KPI level (configure cost live) */
        int nk; mu_lock(&t->mu); nk = (t->kpi >= GPTPS_TUI_KPI_FULL) ? GPTPS_TUI_KPI_MINIMAL : t->kpi + 1; mu_unlock(&t->mu);
        gptps_tui_set_kpi(t, (gptps_tui_kpi)nk); return 3;
    }
    if (key == 'p' || key == 'P') {                 /* toggle pause */
        int nm; mu_lock(&t->mu); nm = (t->mode == GPTPS_TUI_PAUSED) ? GPTPS_TUI_CONTINUOUS : GPTPS_TUI_PAUSED; mu_unlock(&t->mu);
        gptps_tui_set_mode(t, (gptps_tui_mode)nm); return 3;
    }
    if (key == 'k' || key == 'K') { mu_lock(&t->mu); t->scroll += 1; mu_unlock(&t->mu); return 2; } /* scroll to older */
    if (key == 'j' || key == 'J') { mu_lock(&t->mu); if (t->scroll > 0) t->scroll -= 1; mu_unlock(&t->mu); return 2; } /* newer */
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
#endif

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
            fputs("\x1b[H\x1b[2J", t->cfg.out);      /* home + clear */
            gptps_tui_render(t, buf, cap);
            fputs(buf, t->cfg.out); fflush(t->cfg.out);
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
