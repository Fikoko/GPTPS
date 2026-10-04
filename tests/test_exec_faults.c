/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_exec_faults.c - the out-of-process executors when system calls fail.
 *
 * src/exec_oop_posix.c talks to the kernel more than any other core file, and the
 * other tests only ever see those calls succeed. This one fails them, one at a time:
 * every call the executors make to fork, pipe, pipe2, dup2, poll, read, write,
 * close, waitpid, kill, setrlimit and execvp, and fcntl's F_DUPFD forms, which find
 * a descriptor above fd 2. Not failed: fcntl's other commands (on a pipe end the
 * executor has just made, Linux cannot fail F_GETFD, F_SETFD, F_GETFL or an
 * O_NONBLOCK F_SETFL), setpgid, nanosleep, the signal-mask calls, and the cgroup
 * files, whose path is off here (main() unsets GPTPS_CGROUP_PARENT).
 *
 * HOW. The test links the static core with the linker's --wrap for each of those
 * calls; GNU ld and lld both have it. glibc gives two of them another name in some
 * builds - setrlimit64 and fcntl64 under -D_FILE_OFFSET_BITS=64, __fcntl_time64
 * under a 32-bit -D_TIME_BITS=64 - and CMake wraps those spellings as well where
 * the C library has them, so either one is counted. Unarmed, a wrapper passes the
 * call through.
 * Armed, it counts calls per process - this one, the parent, and the child the
 * executor forks, whose counts live in a shared page because the child's copy of
 * everything else dies with it - and fails exactly the Nth call of one kind on one
 * side, the way the kernel would fail it (see k_variants). The armed window is one
 * gptps_step on a MANUAL engine. Nothing else in that window makes these calls, so
 * "the 3rd read" names the same read in every run. Linux only (the CMake guard):
 * the checks below read /proc/self/fd and use PR_SET_CHILD_SUBREAPER.
 *
 * WHAT. Each scenario - OOP and PROGRAM tasks that succeed, fail, crash, time out or
 * are cancelled, and two in a host that closed fds 0 and 1, as a daemon does - first
 * runs undisturbed and counts its calls. Then, for every way a kind of call can
 * fail, the sweep fails its 1st call, its 2nd, and on for as long as a run still
 * reaches the next one, one run each. Every run must:
 *   - give the item exactly one terminal event, and never hang (an alarm watchdog);
 *   - end exactly as the undisturbed run did when the executor can absorb the
 *     failure (EINTR, a short count, close() reporting an error after freeing the
 *     descriptor, a best-effort setrlimit refused), and as a failure with a status
 *     that says so when it cannot - never as a FINISHED carrying the wrong bytes;
 *   - leave /proc/self/fd exactly as it found it: nothing leaked, nothing of
 *     ours closed;
 *   - leave behind no process the executor forked: each must be reaped by the time
 *     shutdown returns, neither a zombie nor still running. The test is a
 *     subreaper, so a grandchild orphaned by a kill is handed to it as well; such
 *     orphans are reaped but not counted (leftover_children() says why).
 * And the census must show every wrapped call, and every way of failing it, at work
 * somewhere: a call a build routes around --wrap fails the test instead of leaving
 * a hole in the sweep.
 *
 * VIRTUAL TIME. A timeout takes at least a second and the sweep runs hundreds of
 * them. Where a scenario's child hangs on purpose, the HAL clock (also wrapped) runs
 * ahead: a poll() that times out waits 1 ms for real and moves the clock on by the
 * rest of the slice it asked for, and each nap of the bounded reap counts for half a
 * second. The executor sees what a real wait would have shown it, minus the wait.
 * Virtual time - and a scenario's cancel - waits for the child to reach the point
 * where it hangs (the OOP task running, the program exec'd): a loaded machine can
 * be slow to schedule it, and a child killed before it got there would make fewer
 * calls than the census counted, at random.
 *
 * WHAT IT CAUGHT. Two ways a failed write() became a FINISHED with the wrong bytes:
 *   - "oop ok, child write #2": the OOP child went on writing its record after a
 *     part of it failed, so the parent read the payload's first 8 bytes as its
 *     length - k_res is built so that this parses - and returned 3 bytes of it;
 *   - "program ok, parent write #N": the PROGRAM pump took any error writing stdin
 *     for EPIPE, closed stdin, and reported what `cat` made of the truncated
 *     payload as a success: anything from 0 to 94208 of its 98304 bytes.
 * "program no-read, parent write #1" changed too, but it was not wrong: the program
 * reads nothing, so the old FINISHED with 0 bytes matched the undisturbed run. It
 * now fails with GPTPS_E_IO, like any stdin the executor could not deliver - the
 * executor cannot know that the program would not have read it.
 *
 * HELPER_PATH is the prog_helper binary (defined by CMake).
 */
#if !defined(_GNU_SOURCE)
#  define _GNU_SOURCE            /* pipe2, prctl and nfds_t under -std=c99 */
#endif
#include "gptps.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>

#ifndef HELPER_PATH
#define HELPER_PATH "./prog_helper"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* ---- the wrapped calls ---------------------------------------------------- */

pid_t    __real_fork(void);
int      __real_pipe(int fds[2]);
int      __real_pipe2(int fds[2], int flags);
int      __real_dup2(int from, int to);
int      __real_poll(struct pollfd *fds, nfds_t n, int ms);
ssize_t  __real_read(int fd, void *buf, size_t n);
ssize_t  __real_write(int fd, const void *buf, size_t n);
int      __real_close(int fd);
pid_t    __real_waitpid(pid_t pid, int *st, int opt);
int      __real_kill(pid_t pid, int sig);
int      __real_setrlimit(int res, const struct rlimit *rl);
int      __real_execvp(const char *file, char *const argv[]);
uint64_t __real_gptps_hal_monotonic_ms(void);

enum { K_FORK, K_PIPE, K_PIPE2, K_DUP2, K_POLL, K_READ, K_WRITE, K_CLOSE,
       K_WAITPID, K_KILL, K_SETRLIMIT, K_EXECVP, K_DUPFD, K_N };
static const char *const k_name[K_N] = {
    "fork", "pipe", "pipe2", "dup2", "poll", "read", "write", "close",
    "waitpid", "kill", "setrlimit", "execvp", "fcntl(F_DUPFD)"
};

enum { PARENT, CHILD };

/* How the armed call fails. */
enum { F_ERR,        /* do nothing, return -1 with the errno */
       F_ERR_DONE,   /* do the call, THEN report -1: Linux close() frees the descriptor
                      * whatever it returns, and a kill() can race the child's own exit */
       F_SHORT,      /* read/write: move at most one byte */
       F_REAPED,     /* waitpid: from this call on, the kernel reaps the child first -
                      * a host with SIGCHLD set to SIG_IGN */
       F_ERR_BOTH }; /* pipe2: fail, and fail the pipe() fallback the same way */

/* What the failure is allowed to do to the run. */
enum { X_ABSORB,     /* nothing: the run ends exactly as an undisturbed one */
       X_FAIL,       /* the item fails, with a status that says so */
       X_LOST };     /* the exit status is gone: an OOP child carries its own over the
                      * pipe (X_ABSORB), a PROGRAM one becomes GPTPS_E_TASK (gptps.h) */

typedef struct { int side, call, how, err, effect; } variant;

/* The ways each kind of call is failed. Mostly what the kernel really returns -
 * EAGAIN or ENOMEM from fork, EMFILE from pipe, ENOMEM from a pipe write that could
 * not get a page - while EIO from read stands for any error the executor cannot
 * expect. Where Linux does the work anyway, so does the wrapper: a failing close()
 * still frees the descriptor, kill() reports ESRCH only once the child is dead, and
 * ECHILD comes from waitpid only with the child really reaped. */
static const variant k_variants[] = {
    /* the parent: the executor itself */
    { PARENT, K_FORK,      F_ERR,      EAGAIN, X_FAIL   },
    { PARENT, K_FORK,      F_ERR,      ENOMEM, X_FAIL   },
    { PARENT, K_PIPE2,     F_ERR,      ENOSYS, X_ABSORB },  /* an old kernel: pipe() instead */
    { PARENT, K_PIPE2,     F_ERR_BOTH, EMFILE, X_FAIL   },  /* out of descriptors: both fail */
    { PARENT, K_POLL,      F_ERR,      EINTR,  X_ABSORB },
    { PARENT, K_POLL,      F_ERR,      ENOMEM, X_FAIL   },
    { PARENT, K_POLL,      F_ERR,      EAGAIN, X_FAIL   },
    { PARENT, K_READ,      F_ERR,      EINTR,  X_ABSORB },
    { PARENT, K_READ,      F_SHORT,    0,      X_ABSORB },
    { PARENT, K_READ,      F_ERR,      EIO,    X_FAIL   },
    { PARENT, K_WRITE,     F_ERR,      EINTR,  X_ABSORB },
    { PARENT, K_WRITE,     F_ERR,      EAGAIN, X_ABSORB },  /* stdin is non-blocking: retry */
    { PARENT, K_WRITE,     F_SHORT,    0,      X_ABSORB },
    { PARENT, K_WRITE,     F_ERR,      ENOMEM, X_FAIL   },  /* no page for the pipe buffer */
    { PARENT, K_CLOSE,     F_ERR_DONE, EINTR,  X_ABSORB },
    { PARENT, K_CLOSE,     F_ERR_DONE, EIO,    X_ABSORB },
    { PARENT, K_WAITPID,   F_ERR,      EINTR,  X_ABSORB },
    { PARENT, K_WAITPID,   F_REAPED,   ECHILD, X_LOST   },
    { PARENT, K_KILL,      F_ERR_DONE, ESRCH,  X_ABSORB },
    { PARENT, K_DUPFD,     F_ERR,      EMFILE, X_FAIL   },  /* no descriptor above fd 2 */
    /* the child, between fork and exec() or _exit() */
    { CHILD,  K_CLOSE,     F_ERR_DONE, EINTR,  X_ABSORB },
    { CHILD,  K_CLOSE,     F_ERR_DONE, EIO,    X_ABSORB },
    { CHILD,  K_DUP2,      F_ERR,      EBUSY,  X_FAIL   },
    { CHILD,  K_DUP2,      F_ERR,      EINTR,  X_FAIL   },
    { CHILD,  K_DUPFD,     F_ERR,      EMFILE, X_FAIL   },  /* the PROGRAM child's hoist */
    { CHILD,  K_SETRLIMIT, F_ERR,      EPERM,  X_ABSORB },  /* the cap is best-effort */
    { CHILD,  K_WRITE,     F_ERR,      EINTR,  X_ABSORB },
    { CHILD,  K_WRITE,     F_SHORT,    0,      X_ABSORB },
    { CHILD,  K_WRITE,     F_ERR,      ENOMEM, X_FAIL   },
    { CHILD,  K_WRITE,     F_ERR,      EPIPE,  X_FAIL   },
    { CHILD,  K_EXECVP,    F_ERR,      ENOENT, X_FAIL   },
    { CHILD,  K_EXECVP,    F_ERR,      ENOMEM, X_FAIL   }
};
#define N_VARIANTS (sizeof k_variants / sizeof k_variants[0])

static struct {
    pid_t          self;          /* this process: its calls are the parent's */
    int            armed;
    const variant *v;             /* the failure to inject, or NULL to only count */
    unsigned       nth;           /* ...at this call of v->call on v->side */
    unsigned       n[K_N];        /* the parent's calls this run */
    int            fired;         /* the parent's injection happened */
    int            reaped;        /* F_REAPED is in force */
    int            pipe_err;      /* F_ERR_BOTH: what the pipe() fallback fails with */
    unsigned       fast;          /* FAST_*: where time may run ahead (see the header) */
    unsigned       cancel_poll;   /* gptps_cancel at the start of this poll, if nonzero */
    unsigned       cancel_wait;   /* ...or of this waitpid - or the first after it at
                                   * which the child is ready (see the header) */
    int            cancelled;     /* that cancel was made */
    gptps         *e;             /* what to cancel */
    gptps_handle   h;
    uint64_t       skew;          /* virtual ms added to the HAL clock */
    unsigned       clock;         /* the parent's reads of the HAL clock this run */
    pid_t          kids[8];       /* children forked this run, for the watchdog */
    unsigned       nkids;
} g;

#define FAST_POLL 1u              /* a poll() that times out */
#define FAST_WAIT 2u              /* a nap of the bounded reap */

/* The child's counts: everything else it writes dies with it. */
static struct shared {
    unsigned n[K_N];
    int      fired;
    int      ready;               /* the child got to work: the OOP task, or exec */
} *g_sh;

static int fi_child(void) { return getpid() != g.self; }

/* Count this call; is it the one to fail? */
static int fi_hit(int call)
{
    int child;
    unsigned n;
    if (!g.armed) return 0;
    child = fi_child();
    if (child) n = __atomic_add_fetch(&g_sh->n[call], 1u, __ATOMIC_SEQ_CST);
    else       n = ++g.n[call];
    if (!g.v || g.v->call != call || g.v->side != child || n != g.nth) return 0;
    if (child) __atomic_store_n(&g_sh->fired, 1, __ATOMIC_SEQ_CST);
    else       g.fired = 1;
    return 1;
}

static int child_ready(void) { return __atomic_load_n(&g_sh->ready, __ATOMIC_SEQ_CST); }

/* A cancel that lands here, as one from another thread could. The step released
 * the engine lock before it called the executor, so this is safe. */
static void fi_cancel_at(int call)
{
    unsigned at = (call == K_POLL) ? g.cancel_poll : g.cancel_wait;
    if (g.armed && at && !g.cancelled && !fi_child() && g.n[call] >= at && child_ready()) {
        g.cancelled = 1;
        (void)gptps_cancel(g.e, g.h);
    }
}

pid_t __wrap_fork(void)
{
    pid_t pid;
    if (fi_hit(K_FORK)) { errno = g.v->err; return -1; }
    pid = __real_fork();
    if (pid > 0 && g.armed && g.nkids < sizeof g.kids / sizeof g.kids[0]) g.kids[g.nkids++] = pid;
    return pid;
}

int __wrap_pipe2(int fds[2], int flags)
{
    if (fi_hit(K_PIPE2)) {
        if (g.v->how == F_ERR_BOTH) g.pipe_err = g.v->err;
        errno = g.v->err;
        return -1;
    }
    return __real_pipe2(fds, flags);
}

int __wrap_pipe(int fds[2])
{
    int hit = fi_hit(K_PIPE);
    if (hit || g.pipe_err) {
        errno = hit ? g.v->err : g.pipe_err;
        g.pipe_err = 0;
        return -1;
    }
    return __real_pipe(fds);
}

int __wrap_dup2(int from, int to)
{
    if (fi_hit(K_DUP2)) { errno = g.v->err; return -1; }
    return __real_dup2(from, to);
}

int __wrap_poll(struct pollfd *fds, nfds_t n, int ms)
{
    int hit = fi_hit(K_POLL), r;
    fi_cancel_at(K_POLL);
    if (hit) { errno = g.v->err; return -1; }
    if (g.armed && (g.fast & FAST_POLL) && ms > 1 && !fi_child()) {
        r = __real_poll(fds, n, 1);
        if (r == 0 && child_ready()) g.skew += (uint64_t)(ms - 1);   /* the rest "passed" */
        return r;
    }
    return __real_poll(fds, n, ms);
}

ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    if (fi_hit(K_READ)) {
        if (g.v->how == F_SHORT) return __real_read(fd, buf, n > 1 ? 1 : n);
        errno = g.v->err;
        return -1;
    }
    return __real_read(fd, buf, n);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    if (fi_hit(K_WRITE)) {
        if (g.v->how == F_SHORT) return __real_write(fd, buf, n > 1 ? 1 : n);
        errno = g.v->err;
        return -1;
    }
    return __real_write(fd, buf, n);
}

int __wrap_close(int fd)
{
    if (fi_hit(K_CLOSE)) {
        if (g.v->how == F_ERR_DONE) (void)__real_close(fd);
        errno = g.v->err;
        return -1;
    }
    return __real_close(fd);
}

pid_t __wrap_waitpid(pid_t pid, int *st, int opt)
{
    int hit = fi_hit(K_WAITPID), lost;
    pid_t r;
    fi_cancel_at(K_WAITPID);
    if (hit && g.v->how == F_REAPED) g.reaped = 1;
    else if (hit) { errno = g.v->err; return -1; }
    if (g.reaped && pid > 0) {
        r = __real_waitpid(pid, &lost, opt);
        if (r == pid) { errno = ECHILD; return -1; }   /* reaped, and its status with it */
    } else {
        r = __real_waitpid(pid, st, opt);
    }
    /* Still running, so the bounded reap naps before it asks again: let each nap
     * count for half a second, and a deadline arrive within a dozen of them. */
    if (r == 0 && g.armed && (g.fast & FAST_WAIT) && !fi_child() && child_ready()) g.skew += 500;
    return r;
}

int __wrap_kill(pid_t pid, int sig)
{
    if (fi_hit(K_KILL)) {
        if (g.v->how == F_ERR_DONE) (void)__real_kill(pid, sig);
        errno = g.v->err;
        return -1;
    }
    return __real_kill(pid, sig);
}

int __wrap_setrlimit(int res, const struct rlimit *rl)
{
    if (fi_hit(K_SETRLIMIT)) { errno = g.v->err; return -1; }
    return __real_setrlimit(res, rl);
}

#if defined(GPTPS_WRAP_SETRLIMIT64)
/* setrlimit, as -D_FILE_OFFSET_BITS=64 names it. Its struct is the 64-bit one; the
 * wrapper only passes the pointer on. */
int __real_setrlimit64(int res, const void *rl);
int __wrap_setrlimit64(int res, const void *rl)
{
    if (fi_hit(K_SETRLIMIT)) { errno = g.v->err; return -1; }
    return __real_setrlimit64(res, rl);
}
#endif

int __wrap_execvp(const char *file, char *const argv[])
{
    if (fi_hit(K_EXECVP)) { errno = g.v->err; return -1; }
    if (g.armed && fi_child()) __atomic_store_n(&g_sh->ready, 1, __ATOMIC_SEQ_CST);
    return __real_execvp(file, argv);
}

/* fcntl's F_DUPFD forms are where the executor finds a descriptor above fd 2 -
 * fd_above_stdio() in the OOP parent, the PROGRAM child's hoist - and EMFILE is how
 * they fail when none is free. Every other command passes through uncounted (the
 * header says why). The executor passes an int or nothing, and nothing else in the
 * core calls fcntl. One wrapper for each name the call has (see the header). */
static int dupfd_hit(int cmd)
{
    if ((cmd != F_DUPFD && cmd != F_DUPFD_CLOEXEC) || !fi_hit(K_DUPFD)) return 0;
    errno = g.v->err;
    return 1;
}

#define WRAP_FCNTL(name)                                                    \
    int __real_##name(int fd, int cmd, ...);                                \
    int __wrap_##name(int fd, int cmd, ...)                                 \
    {                                                                       \
        int arg = 0;                                                        \
        if (cmd != F_GETFD && cmd != F_GETFL) {                             \
            va_list ap;                                                     \
            va_start(ap, cmd);                                              \
            arg = va_arg(ap, int);                                          \
            va_end(ap);                                                     \
        }                                                                   \
        return dupfd_hit(cmd) ? -1 : __real_##name(fd, cmd, arg);           \
    }
WRAP_FCNTL(fcntl)
#if defined(GPTPS_WRAP_FCNTL64)
WRAP_FCNTL(fcntl64)
#endif
#if defined(GPTPS_WRAP_FCNTL_TIME64)
WRAP_FCNTL(__fcntl_time64)
#endif

/* Counted too, so the census sees that the core's clock is this one: in a build
 * where the call is bound inside its own object (the amalgamation), --wrap cannot
 * reach it, and every timeout would take its full time on the wall clock. */
uint64_t __wrap_gptps_hal_monotonic_ms(void)
{
    if (g.armed && !fi_child()) ++g.clock;
    return __real_gptps_hal_monotonic_ms() + g.skew;
}

/* ---- the scenarios -------------------------------------------------------- */

/* A result whose first 8 bytes read as a small length. If the record's header and
 * its payload ever slip against each other, the slip still decodes as a record. */
static const unsigned char k_res[11] = { 3, 0, 0, 0, 0, 0, 0, 0, 'a', 'b', 'c' };

#define BIG_LEN (96u * 1024u)    /* more than a pipe buffer each way */
static unsigned char g_big[BIG_LEN];

static gptps_status t_ok(gptps_ctx *c, void *u)    { (void)u; return gptps_result_set(c, k_res, sizeof k_res); }
static gptps_status t_big(gptps_ctx *c, void *u)   { (void)u; return gptps_result_set(c, g_big, BIG_LEN); }
static gptps_status t_fail(gptps_ctx *c, void *u)  { (void)c; (void)u; return GPTPS_E_TASK; }
static gptps_status t_crash(gptps_ctx *c, void *u) { (void)c; (void)u; raise(SIGKILL); return GPTPS_OK; }
static gptps_status t_hang(gptps_ctx *c, void *u)
{
    (void)c; (void)u;
    __atomic_store_n(&g_sh->ready, 1, __ATOMIC_SEQ_CST);
    for (;;) pause();
    return GPTPS_OK;
}

typedef struct {
    const char          *name;
    gptps_exec_kind      exec;
    gptps_run_fn         run;          /* OOP */
    const char          *mode, *arg;   /* PROGRAM: prog_helper's arguments */
    size_t               plen;         /* payload: the first plen bytes of g_big */
    uint32_t             timeout_s;
    unsigned             fast;         /* FAST_*: the child hangs there on purpose */
    unsigned             cancel_poll, cancel_wait;
    int                  want_kind;    /* how an undisturbed run ends */
    gptps_status         want_status;
    const unsigned char *want_res;     /* ...and with what result, on a FINISHED */
    size_t               want_len;
    int                  daemon;       /* fds 0 and 1 closed for the run, as a daemon has them */
} scenario;

static const scenario k_scenarios[] = {
    { "oop ok",          GPTPS_EXEC_OOP,     t_ok,    NULL,      NULL, 0,       30, 0,         0, 0, GPTPS_EV_FINISHED, GPTPS_OK,          k_res, sizeof k_res, 0 },
    { "oop big",         GPTPS_EXEC_OOP,     t_big,   NULL,      NULL, 0,       30, 0,         0, 0, GPTPS_EV_FINISHED, GPTPS_OK,          g_big, BIG_LEN,      0 },
    { "oop fail",        GPTPS_EXEC_OOP,     t_fail,  NULL,      NULL, 0,       30, 0,         0, 0, GPTPS_EV_FAILED,   GPTPS_E_TASK,      NULL,  0,            0 },
    { "oop crash",       GPTPS_EXEC_OOP,     t_crash, NULL,      NULL, 0,       30, 0,         0, 0, GPTPS_EV_FAILED,   GPTPS_E_TASK,      NULL,  0,            0 },
    { "oop timeout",     GPTPS_EXEC_OOP,     t_hang,  NULL,      NULL, 0,       1,  FAST_POLL, 0, 0, GPTPS_EV_FAILED,   GPTPS_E_TIMEOUT,   NULL,  0,            0 },
    { "oop cancel",      GPTPS_EXEC_OOP,     t_hang,  NULL,      NULL, 0,       0,  FAST_POLL, 2, 0, GPTPS_EV_FAILED,   GPTPS_E_CANCELLED, NULL,  0,            0 },
    { "program ok",      GPTPS_EXEC_PROGRAM, NULL,    "cat",     NULL, BIG_LEN, 30, 0,         0, 0, GPTPS_EV_FINISHED, GPTPS_OK,          g_big, BIG_LEN,      0 },
    /* exits 0 without reading: the parent's write meets a REAL EPIPE, which only
     * means the program wanted no more input - still a success */
    { "program no-read", GPTPS_EXEC_PROGRAM, NULL,    "exit",    "0",  BIG_LEN, 30, 0,         0, 0, GPTPS_EV_FINISHED, GPTPS_OK,          NULL,  0,            0 },
    { "program fail",    GPTPS_EXEC_PROGRAM, NULL,    "exit",    "7",  4096,    30, 0,         0, 0, GPTPS_EV_FAILED,   GPTPS_E_TASK,      NULL,  0,            0 },
    { "program timeout", GPTPS_EXEC_PROGRAM, NULL,    "hang",    NULL, 16,      1,  FAST_POLL, 0, 0, GPTPS_EV_FAILED,   GPTPS_E_TIMEOUT,   NULL,  0,            0 },
    { "program cancel",  GPTPS_EXEC_PROGRAM, NULL,    "hang",    NULL, 16,      0,  FAST_POLL, 2, 0, GPTPS_EV_FAILED,   GPTPS_E_CANCELLED, NULL,  0,            0 },
    /* stdout closed, process alive: the deadline, then a cancel, land in the bounded reap */
    { "eofhang timeout", GPTPS_EXEC_PROGRAM, NULL,    "eofhang", NULL, 16,      5,  FAST_WAIT, 0, 0, GPTPS_EV_FAILED,   GPTPS_E_TIMEOUT,   NULL,  0,            0 },
    { "eofhang cancel",  GPTPS_EXEC_PROGRAM, NULL,    "eofhang", NULL, 16,      0,  0,         0, 3, GPTPS_EV_FAILED,   GPTPS_E_CANCELLED, NULL,  0,            0 },
    /* A host that daemonised: fds 0 and 1 free, so pipe() returns those two. The OOP
     * parent moves its pipe above fd 2 (fd_above_stdio), the PROGRAM child its
     * ends (the hoist before its dup2s); both use fcntl's F_DUPFD, failed here too. */
    { "oop daemon",      GPTPS_EXEC_OOP,     t_ok,    NULL,      NULL, 0,       30, 0,         0, 0, GPTPS_EV_FINISHED, GPTPS_OK,          k_res, sizeof k_res, 1 },
    { "program daemon",  GPTPS_EXEC_PROGRAM, NULL,    "cat",     NULL, 4096,    30, 0,         0, 0, GPTPS_EV_FINISHED, GPTPS_OK,          g_big, 4096,         1 }
};
#define N_SCENARIOS (sizeof k_scenarios / sizeof k_scenarios[0])

/* ---- one run ---------------------------------------------------------------- */

typedef struct {
    int           started, attempts, terminal, selfcancel;
    int           kind;              /* the attempt's FINISHED or FAILED */
    gptps_status  status;
    size_t        len;
    unsigned char res[BIG_LEN];
    unsigned      n[2][K_N];         /* the calls each side made */
    unsigned      clock;             /* the parent's reads of the HAL clock */
    int           fired;             /* the armed failure happened */
    int           fd_ok;             /* /proc/self/fd is as it was before the run */
    int           kids;              /* processes the executor forked, still there after shutdown */
    int           orphans;           /* grandchildren handed to the subreaper (not judged) */
    int           hung;              /* the watchdog had to step in */
} run;

static run g_run;                    /* the run in progress: on_ev fills it in */

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    switch (ev->kind) {
    case GPTPS_EV_STARTED:  g_run.started++; break;
    case GPTPS_EV_FINISHED:
        g_run.attempts++; g_run.terminal++;
        g_run.kind = GPTPS_EV_FINISHED; g_run.status = ev->status;
        g_run.len = ev->result_len;
        if (ev->result && ev->result_len <= sizeof g_run.res) memcpy(g_run.res, ev->result, ev->result_len);
        break;
    case GPTPS_EV_FAILED:
        g_run.attempts++;
        g_run.kind = GPTPS_EV_FAILED; g_run.status = ev->status; g_run.len = 0;
        if (ev->status == GPTPS_E_CANCELLED) g_run.terminal++;   /* terminal in its own right */
        if (ev->flags & GPTPS_EV_FLAG_SELF_CANCELLED) g_run.selfcancel++;
        break;
    case GPTPS_EV_DROPPED:
    case GPTPS_EV_DEAD_LETTERED: g_run.terminal++; break;  /* after a FAILED: max_retries 0 */
    default: break;
    }
}

/* An OOP task's memory cap: large enough that the executor applies RLIMIT_AS (16 MiB
 * floor), so setrlimit is on the path, and never binding - a sanitizer's own mappings
 * run to tens of TiB on a 64-bit host - while a 32-bit rlim_t still holds it. A
 * PROGRAM task runs uncapped, as in the other tests: a sanitizer runtime in the
 * program it execs refuses to start under any cap (TSan re-execs to lift it). */
static uint64_t memcap(void) { return sizeof(void *) >= 8 ? 1ull << 47 : 3ull << 30; }

#define FD_MAX 1024
static void fd_snapshot(unsigned char *set)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *de;
    int dfd;
    memset(set, 0, FD_MAX);
    if (!d) return;
    dfd = dirfd(d);
    while ((de = readdir(d)) != NULL) {
        int fd;
        if (de->d_name[0] == '.') continue;
        fd = atoi(de->d_name);
        if (fd != dfd && fd >= 0 && fd < FD_MAX) set[fd] = 1;
    }
    closedir(d);
}

static char g_desc[160];             /* the run in progress, for messages */
static int  g_verbose;
static int  g_outfd = 1;             /* the watchdog's stdout: a copy above fd 2, since a
                                      * daemon scenario runs with fd 1 closed or reused */
static volatile sig_atomic_t g_hung;

/* SIGKILL the run's children, and the process group each PROGRAM child leads - but only
 * one that is still a live child of ours: a reaped pid may already belong to some other
 * process on the machine. waitpid() is async-signal-safe, so the watchdog can use this. */
static void kill_kids(void)
{
    unsigned i;
    int st;
    for (i = 0; i < g.nkids; ++i) {
        if (__real_waitpid(g.kids[i], &st, WNOHANG) != 0) continue;   /* reaped, or not ours */
        (void)__real_kill(-g.kids[i], SIGKILL);
        (void)__real_kill(g.kids[i], SIGKILL);
    }
}

/* The watchdog. A hang is reported, then the run's children are killed - which frees
 * an executor stuck waiting on one - and the run is judged a failure, so one hang does
 * not hide the rest of the sweep. A run still stuck after that is not waiting on a
 * child at all, and only exiting ends it. */
#define WATCHDOG_S 20
static void on_alarm(int sig)
{
    static const char msg[] = "FAIL: watchdog - this run hung: ";
    (void)sig;
    if (g_hung) _exit(3);
    g_hung = 1;
    (void)__real_write(g_outfd, msg, sizeof msg - 1);
    (void)__real_write(g_outfd, g_desc, strlen(g_desc));
    (void)__real_write(g_outfd, "\n", 1);
    kill_kids();
    alarm(5);
}

/* SIGKILL every process whose parent is this one: the executor's children and, this
 * being a subreaper, any orphan of theirs that was handed to it - which kill_kids()
 * knows nothing about. */
static void kill_all_children(void)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    long self = (long)getpid();
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        char path[sizeof de->d_name + 16], line[512], *p;
        long ppid = 0;
        FILE *f;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/stat", de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        /* "pid (comm) state ppid ...": comm may hold anything, ')' included */
        if (fgets(line, sizeof line, f) && (p = strrchr(line, ')')) != NULL
            && sscanf(p + 1, " %*c %ld", &ppid) == 1 && ppid == self)
            (void)__real_kill((pid_t)atol(de->d_name), SIGKILL);
        fclose(f);
    }
    closedir(d);
}

static int is_kid(pid_t pid)
{
    unsigned i;
    for (i = 0; i < g.nkids; ++i) if (g.kids[i] == pid) return 1;
    return 0;
}

/* How many of the processes the executor forked this run are still there after
 * shutdown: zombies it never reaped, or processes still running. Only those count.
 *
 * Any other child of this process is an orphan, handed over because the test is a
 * subreaper; the executor neither made it nor owes us its exit. One such orphan is
 * real, and a sanitizer makes it: at exit, LeakSanitizer in an ASan-built
 * prog_helper clones a tracer task, which shares the program's process group. When
 * the executor SIGKILLs the group in the middle of that check, the tracer dies with
 * it and comes to us (a host that is not a subreaper never sees it: init reaps it).
 * Counted, it failed ASan runs at random as "a child left behind".
 *
 * Every child, counted or not, is killed and reaped here, without ever blocking -
 * an orphan can be handed over mid-cleanup - so a failing run does not hang the
 * test or spill into the next run. */
static int leftover_children(int *orphans)
{
    struct timespec nap;
    unsigned i;
    int st, n = 0, tries;
    pid_t r;
    for (i = 0; i < g.nkids; ++i)               /* the executor reaped it: ECHILD */
        if (__real_waitpid(g.kids[i], &st, WNOHANG) >= 0) ++n;   /* a zombie, or running */
    for (tries = 0; tries < 200; ++tries) {
        while ((r = __real_waitpid(-1, &st, WNOHANG)) > 0)
            if (!is_kid(r)) ++*orphans;
        if (r < 0 && errno != EINTR) break;     /* ECHILD: no child left */
        kill_all_children();
        nap.tv_sec = 0; nap.tv_nsec = 10L * 1000000L;
        nanosleep(&nap, NULL);
    }
    return n;
}

static unsigned g_runs, g_fired, g_orphans;
static unsigned g_seen[2][K_N], g_clock;    /* every call the wrappers saw, all runs */

/* One item of the scenario, run by one gptps_step with `v` armed at its nth call
 * (v NULL: only count). The result is left in g_run. */
static void run_once(const scenario *sc, const variant *v, unsigned nth)
{
    static unsigned char fd0[FD_MAX], fd1[FD_MAX];
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    gptps_handle h = 0;
    const char *argv[4];
    size_t ran = 0;
    int guard = 0, k, saved[2] = { -1, -1 }, low_free = 1, step_ok = 1, shut_ok;

    memset(&g_run, 0, sizeof g_run);
    g_run.kind = -1;
    memset(g_sh, 0, sizeof *g_sh);
    fd_snapshot(fd0);

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 2 * memcap();
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    gptps_set_event_cb(e, on_ev, NULL);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "t"; d.exec = sc->exec;
    if (sc->exec == GPTPS_EXEC_OOP) d.run = sc->run;
    else {
        argv[0] = HELPER_PATH; argv[1] = sc->mode; argv[2] = sc->arg; argv[3] = NULL;
        d.argv = argv;
    }
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = (sc->exec == GPTPS_EXEC_OOP) ? memcap() : 0;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = sc->timeout_s;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DROP;     /* max_retries 0: one attempt */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "t", g_big, sc->plen, &h) == GPTPS_OK);

    memset(g.n, 0, sizeof g.n);
    g.v = v; g.nth = nth; g.fired = 0; g.reaped = 0; g.pipe_err = 0;
    g.fast = sc->fast; g.cancel_poll = sc->cancel_poll; g.cancel_wait = sc->cancel_wait;
    g.e = e; g.h = h; g.nkids = 0; g.clock = 0; g.cancelled = 0;
    if (!v) snprintf(g_desc, sizeof g_desc, "%s, undisturbed", sc->name);
    else    snprintf(g_desc, sizeof g_desc, "%s, %s %s #%u %s%s", sc->name,
                     v->side == CHILD ? "child" : "parent", k_name[v->call], nth,
                     v->how == F_SHORT ? "short" : v->how == F_REAPED ? "reaped" : strerror(v->err),
                     v->how == F_ERR_BOTH ? " (and pipe)" : "");
    /* A daemon's fds 0 and 1: closed, copies parked above fd 2 to restore after. No
     * printf until then - fd 1 may be one of the executor's pipes - so the step's and
     * the shutdown's results are checked after the restore. */
    if (sc->daemon)
        for (k = 0; k < 2; ++k) { saved[k] = fcntl(k, F_DUPFD_CLOEXEC, 3); close(k); }
    g_hung = 0;
    alarm(WATCHDOG_S);
    g.armed = 1;
    do { if (gptps_step(e, &ran) != GPTPS_OK) step_ok = 0; } while (ran && ++guard < 4);
    g.armed = 0;
    shut_ok = gptps_shutdown(e) == GPTPS_OK;
    alarm(0);
    g.reaped = 0; g.pipe_err = 0;
    if (sc->daemon)
        for (k = 0; k < 2; ++k) {
            /* Still closed, or the run left a descriptor there - which the restore
             * would close without a word. */
            if (fcntl(k, F_GETFD) != -1 || errno != EBADF) low_free = 0;
            if (saved[k] >= 0) { dup2(saved[k], k); close(saved[k]); }
        }
    CHECK(saved[0] >= 0 || !sc->daemon);
    CHECK(saved[1] >= 0 || !sc->daemon);
    CHECK(step_ok);
    CHECK(shut_ok);

    memcpy(g_run.n[PARENT], g.n, sizeof g.n);
    memcpy(g_run.n[CHILD], g_sh->n, sizeof g_sh->n);
    g_run.clock = g.clock;
    g_run.fired = g.fired || g_sh->fired;
    g_run.hung = g_hung;
    fd_snapshot(fd1);
    g_run.fd_ok = low_free && memcmp(fd0, fd1, FD_MAX) == 0;
    g_run.kids = leftover_children(&g_run.orphans);
    ++g_runs;
    if (g_run.fired) ++g_fired;
    g_orphans += (unsigned)g_run.orphans;
    g_clock += g_run.clock;
    for (k = 0; k < K_N; ++k) {
        g_seen[PARENT][k] += g_run.n[PARENT][k];
        g_seen[CHILD][k] += g_run.n[CHILD][k];
    }
}

static const char *kind_name(int k)
{ return k == GPTPS_EV_FINISHED ? "FINISHED" : k == GPTPS_EV_FAILED ? "FAILED" : "none"; }

/* Judge the run just made (g_run) against the undisturbed one. */
static void judge(const scenario *sc, const variant *v, const run *base)
{
    const run *got = &g_run;
    int effect = (v && got->fired) ? v->effect : X_ABSORB, ok;
    if (effect == X_LOST && sc->exec == GPTPS_EXEC_OOP) effect = X_ABSORB;

    ok = got->started == 1 && got->attempts == 1 && got->terminal == 1 && got->selfcancel == 0;
    if (effect == X_ABSORB) {
        ok = ok && got->kind == base->kind && got->status == base->status && got->len == base->len
                && got->len <= sizeof got->res && memcmp(got->res, base->res, got->len) == 0;
    } else {
        /* The status the failure earns - or the one the run was ending with anyway (a
         * cancel that landed first, say). The parent's own calls failing is an I/O
         * error; the child's means it died or tore its record. One wart is pinned
         * rather than allowed in general: read_all() cannot tell a failed read from
         * EOF, so the OOP parent reports a failed read of the record's header as the
         * child dying first, GPTPS_E_TASK. A pipe read cannot fail that way in
         * practice; it does not say the task failed either. */
        gptps_status s = got->status;
        int sensible = base->kind == GPTPS_EV_FAILED && s == base->status;
        if (effect == X_LOST)       sensible |= s == GPTPS_E_TASK;   /* gptps.h: the exit status is gone */
        else if (v->side == CHILD)  sensible |= s == GPTPS_E_TASK || s == GPTPS_E_IO;
        else                        sensible |= s == GPTPS_E_IO || s == GPTPS_E_NOMEM
                                             || (s == GPTPS_E_TASK && v->call == K_READ && sc->exec == GPTPS_EXEC_OOP);
        ok = ok && got->kind == GPTPS_EV_FAILED && sensible;
    }
    ok = ok && got->fd_ok && got->kids == 0 && !got->hung;
    if (!ok || g_verbose) {
        printf("%s %s: %s/%s, %u bytes (undisturbed: %s/%s, %u bytes); %d started, %d ended, %d terminal%s%s%s%s%s\n",
               ok ? "ok  " : "FAIL", g_desc, kind_name(got->kind), gptps_strerror(got->status),
               (unsigned)got->len, kind_name(base->kind), gptps_strerror(base->status), (unsigned)base->len,
               got->started, got->attempts, got->terminal,
               got->fd_ok ? "" : "; descriptors leaked or lost",
               got->kids ? "; a child left behind" : "",
               got->hung ? "; hung" : "",
               got->orphans ? "; an orphaned grandchild reaped" : "",
               got->fired ? "" : " (never reached)");
    }
    if (!ok) ++fails;
}

static int g_vfired[sizeof k_variants / sizeof k_variants[0]];   /* each variant, in any scenario */

static void sweep(const scenario *sc)
{
    static run base;
    const unsigned *pc = base.n[PARENT], *cc = base.n[CHILD];
    unsigned i, n;
    int census_fails;

    run_once(sc, NULL, 0);
    base = g_run;
    /* the undisturbed run must itself be right, or nothing below means anything */
    CHECK(base.kind == sc->want_kind && base.status == sc->want_status);
    CHECK(base.len == sc->want_len && (!sc->want_len || memcmp(base.res, sc->want_res, sc->want_len) == 0));
    judge(sc, NULL, &base);
    if (g_verbose) {
        printf("census %s: parent", sc->name);
        for (i = 0; i < K_N; ++i) if (pc[i]) printf(" %s %u", k_name[i], pc[i]);
        printf("; child");
        for (i = 0; i < K_N; ++i) if (cc[i]) printf(" %s %u", k_name[i], cc[i]);
        printf("\n");
    }
    /* A wrapper that never sees a call cannot fail it: a build that routes one
     * around --wrap must not pass quietly - a fortified read() becoming __read_chk,
     * say, or a name for a call that no wrapper has. What each scenario must make
     * for certain is checked here; main() checks that every wrapped call, and every
     * way of failing one, was at work somewhere. */
    census_fails = fails;
    CHECK(pc[K_FORK] == 1 && pc[K_PIPE2] >= 1 && pc[K_POLL] >= 1 && pc[K_WAITPID] >= 1 && pc[K_CLOSE] >= 2);
    CHECK(base.clock >= 1);                            /* the wrapped clock is the core's */
    if (sc->exec == GPTPS_EXEC_OOP && !sc->fast) CHECK(pc[K_READ] >= 1);
    if (sc->exec == GPTPS_EXEC_OOP && sc->want_len) CHECK(cc[K_WRITE] >= 3);
    if (sc->exec == GPTPS_EXEC_OOP) CHECK(cc[K_SETRLIMIT] == 1);   /* the memory cap */
    if (sc->exec == GPTPS_EXEC_PROGRAM) CHECK(cc[K_DUP2] == 2 && cc[K_EXECVP] == 1);
    if (sc->exec == GPTPS_EXEC_PROGRAM && sc->want_len) CHECK(pc[K_WRITE] >= 1 && pc[K_READ] >= 1);
    if (sc->want_status == GPTPS_E_TIMEOUT || sc->want_status == GPTPS_E_CANCELLED)
        CHECK(pc[K_KILL] >= 1);                        /* it ends with the child killed */
    if (sc->daemon && sc->exec == GPTPS_EXEC_OOP) CHECK(pc[K_DUPFD] == 2);       /* both pipe ends */
    if (sc->daemon && sc->exec == GPTPS_EXEC_PROGRAM) CHECK(cc[K_DUPFD] == 2);   /* stdin's pipe */
    if (fails != census_fails) printf("  (the census of %s)\n", sc->name);

    for (i = 0; i < N_VARIANTS; ++i) {
        const variant *v = &k_variants[i];
        unsigned total = base.n[v->side][v->call], tries = 0;
        int any = 0;
        if (!total) continue;                 /* this scenario never makes that call */
        /* Every call the census counted, and on for as long as runs keep reaching the
         * next one: how many polls or reads a run makes depends on scheduling. */
        for (n = 1; n <= total || (g_run.fired && n < 4 * total + 8); ++n) {
            run_once(sc, v, n);
            any |= g_run.fired;
            judge(sc, v, &base);
        }
        /* A call the census saw has to be failable, or the sweep proves nothing. Even
         * the first one can hang on scheduling - a child that exits before the parent
         * gets to write its stdin - so it gets two more chances. */
        while (!any && tries++ < 2) {
            run_once(sc, v, 1);
            any = g_run.fired;
            judge(sc, v, &base);
        }
        if (!any) {
            printf("FAIL %s: %s %s was counted but never failed\n", sc->name,
                   v->side == CHILD ? "the child's" : "the parent's", k_name[v->call]);
            ++fails;
        }
        g_vfired[i] |= any;
    }
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    unsigned i;

    g_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    setvbuf(stdout, NULL, _IONBF, 0);    /* the watchdog writes around stdio */
    /* fds 0-2 open, so only a daemon scenario's pipes can land below 3 */
    for (i = 0; i < 3; ++i)
        if (fcntl((int)i, F_GETFD) == -1 && errno == EBADF) {
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0 && fd != (int)i) { dup2(fd, (int)i); close(fd); }
        }
    g_outfd = fcntl(1, F_DUPFD_CLOEXEC, 3);
    if (g_outfd < 0) g_outfd = 1;
    g.self = getpid();
    unsetenv("GPTPS_CGROUP_PARENT");     /* the RLIMIT_AS path, every time */
    g_sh = (struct shared *)mmap(NULL, sizeof *g_sh, PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(g_sh != MAP_FAILED);
    if (g_sh == MAP_FAILED) return 1;
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
    for (i = 0; i < BIG_LEN; ++i) g_big[i] = (unsigned char)((i * 1103515245u + 12345u) >> 16);

    for (i = 0; i < N_SCENARIOS; ++i) sweep(&k_scenarios[i]);

    /* Every wrapped call seen, and every way of failing one injected, somewhere. */
    for (i = 0; i < K_N; ++i)
        if (!g_seen[PARENT][i] && !g_seen[CHILD][i]) {
            printf("FAIL: no run made a %s the wrapper saw - is it routed around --wrap?\n", k_name[i]);
            ++fails;
        }
    if (!g_clock) { printf("FAIL: the HAL clock was never the wrapped one\n"); ++fails; }
    for (i = 0; i < N_VARIANTS; ++i)
        if (!g_vfired[i]) {
            const variant *v = &k_variants[i];
            printf("FAIL: %s %s was never failed (%s) in any scenario\n",
                   v->side == CHILD ? "the child's" : "the parent's", k_name[v->call],
                   v->how == F_SHORT ? "short" : v->how == F_REAPED ? "reaped" : strerror(v->err));
            ++fails;
        }

    printf("%u runs, %u with a failure injected", g_runs, g_fired);
    if (g_orphans) printf("; %u orphaned grandchildren reaped, not judged", g_orphans);
    printf("\n");
    if (fails) { printf("%d exec-fault check(s) FAILED\n", fails); return 1; }
    printf("all exec-fault checks passed\n");
    return 0;
}
