/*
 * gptps_xport.c - worker-process transport (scale-out) built on POSIX IPC.
 *
 * open() creates N socketpairs and forks a worker for each; a worker loops reading
 * [tlen][task][plen][payload], runs the handler, and writes back [status][rlen][res].
 * submit() round-robins to a worker, holds that worker's mutex for the one blocking
 * round-trip, and returns the result. close() drops each link (EOF -> the worker
 * exits) and reaps. No core change; everything here is POSIX + the public header.
 */
#if defined(_WIN32)
/* fork()/socketpair() have no Win32 equivalent; this reference add-on is POSIX-only
 * (like GPTPS_EXEC_OOP). It compiles to nothing on Windows. */
#else

#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif

#include "gptps_xport.h"
#include "addon_compat.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0   /* macOS/BSD: rely on SO_NOSIGPIPE (set below) instead */
#endif

/* Sanity cap on any single framed message (task name / payload / result). A same-
 * machine worker runs your own forked handler, so this is defense-in-depth against a
 * buggy peer rather than a trust boundary - but it bounds a bad length field to a
 * clean GPTPS_E_IO instead of a multi-GB malloc, and is what a cross-host TCP variant
 * of this protocol would require anyway. */
#define GPTPS_XPORT_MAX_MSG ((uint64_t)256u * 1024u * 1024u)

typedef struct {
    pid_t     pid;
    int       fd;      /* parent side of the socketpair to this worker */
    apx_mutex mu;      /* serialises one round-trip per worker */
} xport_worker;

struct gptps_xport {
    xport_worker *w;
    size_t        n;
    apx_mutex     cursor_lock;
    uint64_t      cursor;
};

/* ---- framed socket I/O (EINTR-safe; SIGPIPE suppressed per-call) ---- */
static int sock_write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf; size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}
static int sock_read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf; size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;   /* peer closed */
        off += (size_t)r;
    }
    return 0;
}

/* ---- the worker loop (runs in the forked child; never returns) ---- */
static void xport_worker_main(int fd, gptps_xport_run_fn run, void *ud)
{
    for (;;) {
        uint32_t tlen = 0; uint64_t plen = 0;
        char *task = NULL; void *payload = NULL, *res = NULL;
        size_t rlen = 0;
        int32_t st32;
        uint64_t rl;

        if (sock_read_all(fd, &tlen, sizeof tlen) != 0) break;     /* EOF => shut down */
        if (tlen > GPTPS_XPORT_MAX_MSG) break;                     /* bad length: drop the link */
        task = (char *)malloc((size_t)tlen + 1);
        if (!task || sock_read_all(fd, task, tlen) != 0) { free(task); break; }
        task[tlen] = 0;
        if (sock_read_all(fd, &plen, sizeof plen) != 0) { free(task); break; }
        if (plen > GPTPS_XPORT_MAX_MSG) { free(task); break; }
        if (plen) {
            payload = malloc((size_t)plen);
            if (!payload || sock_read_all(fd, payload, (size_t)plen) != 0) { free(task); free(payload); break; }
        }

        st32 = (int32_t)run(task, payload, (size_t)plen, &res, &rlen, ud);
        rl = (uint64_t)rlen;
        if (sock_write_all(fd, &st32, sizeof st32) != 0 ||
            sock_write_all(fd, &rl, sizeof rl) != 0 ||
            (rlen && sock_write_all(fd, res, rlen) != 0)) {
            free(res); free(payload); free(task); break;
        }
        free(res); free(payload); free(task);
    }
    close(fd);
    _exit(0);
}

static void set_nosigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)   /* macOS/BSD per-socket SIGPIPE suppression */
    int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

gptps_xport *gptps_xport_open(size_t nworkers, gptps_xport_run_fn run, void *user_data)
{
    gptps_xport *xp;
    size_t i;

    if (nworkers < 1 || !run) return NULL;
    xp = (gptps_xport *)calloc(1, sizeof *xp);
    if (!xp) return NULL;
    xp->w = (xport_worker *)calloc(nworkers, sizeof *xp->w);
    if (!xp->w) { free(xp); return NULL; }
    apx_mutex_init(&xp->cursor_lock);

    for (i = 0; i < nworkers; ++i) {
        int sp[2];
        pid_t pid;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) { gptps_xport_close(xp); return NULL; }
        /* suppress SIGPIPE on BOTH ends before the fork, so the worker's REPLY send
         * (macOS/BSD, where MSG_NOSIGNAL is a no-op) also gets EPIPE instead of a
         * signal if the parent's end has closed - not just the parent's request send. */
        set_nosigpipe(sp[0]); set_nosigpipe(sp[1]);
        pid = fork();
        if (pid < 0) { close(sp[0]); close(sp[1]); gptps_xport_close(xp); return NULL; }
        if (pid == 0) {
            /* CHILD i: keep only sp[1]; drop this pair's parent end and every prior
             * worker's parent fd we inherited, so no stray fd holds a link open. */
            size_t j;
            close(sp[0]);
            for (j = 0; j < i; ++j) close(xp->w[j].fd);
            xport_worker_main(sp[1], run, user_data);   /* never returns */
        }
        /* PARENT */
        close(sp[1]);
        xp->w[i].pid = pid; xp->w[i].fd = sp[0];
        apx_mutex_init(&xp->w[i].mu);
        xp->n = i + 1;                                   /* keep n consistent for a partial teardown */
    }
    return xp;
}

size_t gptps_xport_count(gptps_xport *xp) { return xp ? xp->n : 0; }

static size_t next_rr(gptps_xport *xp)
{
    size_t idx;
    apx_mutex_lock(&xp->cursor_lock);
    idx = (size_t)(xp->cursor++ % xp->n);
    apx_mutex_unlock(&xp->cursor_lock);
    return idx;
}

gptps_status gptps_xport_submit(gptps_xport *xp, const char *task,
                                const void *payload, size_t len,
                                void **out_result, size_t *out_len,
                                gptps_status *out_task_status)
{
    xport_worker *w;
    uint32_t tlen;
    uint64_t plen, rl = 0;
    int32_t st32 = (int32_t)GPTPS_E_IO;
    void *res = NULL;
    int ok;

    if (out_result) *out_result = NULL;
    if (out_len) *out_len = 0;
    if (out_task_status) *out_task_status = GPTPS_E_IO;
    if (!xp || !task) return GPTPS_E_INVAL;

    tlen = (uint32_t)strlen(task);
    plen = (uint64_t)len;
    w = &xp->w[next_rr(xp)];

    apx_mutex_lock(&w->mu);
    ok = sock_write_all(w->fd, &tlen, sizeof tlen) == 0 &&
         sock_write_all(w->fd, task, tlen) == 0 &&
         sock_write_all(w->fd, &plen, sizeof plen) == 0 &&
         (len == 0 || sock_write_all(w->fd, payload, len) == 0) &&
         sock_read_all(w->fd, &st32, sizeof st32) == 0 &&
         sock_read_all(w->fd, &rl, sizeof rl) == 0;
    if (ok && rl > GPTPS_XPORT_MAX_MSG) ok = 0;   /* bad reply length: fail, don't malloc it */
    if (ok && rl) {
        res = malloc((size_t)rl);
        if (!res || sock_read_all(w->fd, res, (size_t)rl) != 0) { free(res); res = NULL; ok = 0; }
    }
    apx_mutex_unlock(&w->mu);

    if (!ok) { free(res); return GPTPS_E_IO; }   /* worker died / link broke */
    if (out_task_status) *out_task_status = (gptps_status)st32;
    if (out_result) *out_result = res; else free(res);
    if (out_len) *out_len = (size_t)rl;
    return GPTPS_OK;
}

void gptps_xport_close(gptps_xport *xp)
{
    size_t i;
    if (!xp) return;
    for (i = 0; i < xp->n; ++i) {
        close(xp->w[i].fd);                 /* worker sees EOF -> exits */
    }
    for (i = 0; i < xp->n; ++i) {
        int st; while (waitpid(xp->w[i].pid, &st, 0) < 0 && errno == EINTR) { }
        apx_mutex_destroy(&xp->w[i].mu);
    }
    apx_mutex_destroy(&xp->cursor_lock);
    free(xp->w);
    free(xp);
}

#endif /* !_WIN32 */
