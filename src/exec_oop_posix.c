/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * exec_oop_posix.c - out-of-process executor (T13, POSIX).
 *
 * fork() the engine, run the task in the child under an OS memory cap
 * (setrlimit RLIMIT_AS), and stream [status][len][bytes] back over a pipe. The
 * PARENT enforces the deadline with poll() + a hard SIGKILL - the genuinely
 * enforced path that the cooperative in-process executor cannot offer.
 *
 * Caveats (documented):
 *  - fork() in a multithreaded process: only the calling worker survives in the
 *    child. OOP tasks must be fork-safe / self-contained (CPU/memory-bound work,
 *    or untrusted code you want isolated + killable). glibc keeps malloc fork-safe.
 *  - RLIMIT_AS caps VIRTUAL address space, not RSS - a blunt approximation. The
 *    accurate answer (cgroups v2 memory.max) is a later increment; only applied
 *    when mem_cap is large enough (>= floor) to leave room for the base image.
 *  - timeout_s==0 => no deadline of its own, but NOT unstoppable: the parent polls
 *    the cancel flag in bounded slices, so gptps_cancel / task removal / the
 *    shutdown grace still hard-kill the child. Nothing here waits forever.
 *
 * Measurements (docs/MEASUREMENTS.md): the child is collected with wait4(), which
 * returns its resource usage in the same call, and in cgroup mode the job's own
 * cgroup files are read before the cgroup is removed. Both executors report them
 * through the engine's gptps_exec_meter, best method first.
 */

/* feature-test macros before any system header (see hal_posix.c) */
#if !defined(_WIN32) /* POSIX backend; compiles to nothing on Windows (exec_win.c is used) */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif

#include "gptps.h"
#include "gptps_internal.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>   /* pthread_sigmask: per-thread SIGPIPE block (program executor) */
#include <time.h>      /* struct timespec for the sigtimedwait drain */
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#  include <libproc.h>   /* proc_pidinfo: a running child's resident memory (samples) */
#endif

/* wait4() hands back the reaped child's struct rusage. Not POSIX, but every system
 * this backend targets has it; elsewhere children are reaped with waitpid() and
 * nothing is measured (docs/MEASUREMENTS.md, rule 2: measured, or absent). */
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#  define GPTPS_HAVE_WAIT4 1
#else
#  define GPTPS_HAVE_WAIT4 0
#endif

#define GPTPS_OOP_MEMCAP_FLOOR (16ull * 1024ull * 1024ull) /* below this, a mem cap is meaningless */

/* Max bytes either enforced executor will buffer from a child. The OOP child runs
 * our own code, but a torn or corrupted record can still declare an arbitrary
 * length - and on a 32-bit host `(size_t)len64` would silently TRUNCATE, so the
 * parent would allocate a short buffer and then read the rest of the record as if
 * it were the next one. Both executors share one bound. */
#define GPTPS_EXEC_RESULT_CAP (16u * 1024u * 1024u)

/* How long a child that has finished talking to us gets to exit on its own before
 * we escalate to SIGKILL (see reap_bounded). */
#define GPTPS_EXEC_EXIT_GRACE_MS 2000

/* waitpid(), plus the child's resource usage where the system reports it: *have_ru
 * is set when `ru` was filled for the child this call reaped. */
static pid_t wait_child(pid_t pid, int *wstatus, int options, struct rusage *ru, int *have_ru)
{
#if GPTPS_HAVE_WAIT4
    pid_t r = wait4(pid, wstatus, options, ru);
    if (r == pid) *have_ru = 1;
    return r;
#else
    (void)ru; (void)have_ru;
    return waitpid(pid, wstatus, options);
#endif
}

/* Reap `pid` WITHOUT blocking forever. A child that closes its stdout but keeps
 * running - or ignores every signal short of SIGKILL - would otherwise pin this
 * worker inside waitpid(), which is exactly the hang these executors promise never
 * to have. Polls with WNOHANG in short slices, honouring the deadline and the
 * cancel flag, then escalates to SIGKILL (unblockable, so the loop terminates).
 * `group` selects kill(-pid) for a child that leads its own process group.
 * `*reaped` reports whether *wstatus was actually filled in: a host that runs
 * `signal(SIGCHLD, SIG_IGN)` (or a wait-any reaper) auto-reaps our children, so
 * waitpid() fails with ECHILD and wstatus keeps its initialiser. WIFEXITED(0) is
 * then true with exit status 0 - i.e. the caller would read a FAILING program as a
 * success. Callers must refuse to interpret wstatus when *reaped is 0.
 * Returns GPTPS_OK if the child exited on its own, else why it had to be killed. */
static gptps_status reap_bounded(pid_t pid, int *wstatus, int *reaped, int group,
                                 uint64_t deadline, const uint32_t *cancel,
                                 struct rusage *ru, int *have_ru)
{
    gptps_status why = GPTPS_OK;
    int waited = 0;
    *reaped = 0;
    for (;;) {
        struct timespec ts;
        pid_t r = wait_child(pid, wstatus, WNOHANG, ru, have_ru);
        if (r == pid) { *reaped = 1; return why; }
        /* Not an error we can act on (the host already reaped it): keep returning
         * GPTPS_OK - both call sites map a non-OK return onto "we had to kill it",
         * which would turn every successful task into a spurious failure. */
        if (r < 0 && errno != EINTR) return why;
        if (why == GPTPS_OK) {
            uint64_t now = gptps_hal_monotonic_ms();
            if (deadline && now >= deadline)              why = GPTPS_E_TIMEOUT;
            else if (cancel && gptps_hal_load_acquire_u32(cancel))    why = GPTPS_E_CANCELLED;
            else if (waited >= GPTPS_EXEC_EXIT_GRACE_MS)  why = GPTPS_E_TIMEOUT;
            if (why != GPTPS_OK) { if (group) kill(-pid, SIGKILL); else kill(pid, SIGKILL); }
        }
        ts.tv_sec = 0; ts.tv_nsec = 5L * 1000000L;   /* 5ms */
        nanosleep(&ts, NULL);
        waited += 5;
    }
}

/* Create a pipe with both ends close-on-exec so a concurrently-forked child that
 * goes on to exec() (a PROGRAM child) cannot inherit and pin open another
 * executor's pipe ends - which would hang that executor's parent waiting for EOF.
 * O_CLOEXEC only fires AT exec(), so it does nothing for the OOP child, which
 * never execs; that case is handled by the fd registry below. */
static int make_pipe_cloexec(int fds[2])
{
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(fds, O_CLOEXEC) == 0) return 0;
    /* fall through if pipe2 is unavailable on this (old) kernel */
#endif
    if (pipe(fds) != 0) return -1;
    (void)fcntl(fds[0], F_SETFD, fcntl(fds[0], F_GETFD) | FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, fcntl(fds[1], F_GETFD) | FD_CLOEXEC);
    return 0;
}

/* ---- executor fd registry -------------------------------------------------
 * The OOP child never exec()s - it runs the host's task function in-process - so
 * FD_CLOEXEC cannot protect anyone from it. Every descriptor open at fork time
 * stays open in that child for the WHOLE OOP task, including a concurrent PROGRAM
 * executor's stdin write end: `cat` then never sees EOF and that executor pumps to
 * its deadline, returning GPTPS_E_TIMEOUT with an empty result instead of the
 * payload. (It equally swallows POLLHUP on another OOP executor's write end, which
 * turns a child that crashed early into a full-timeout E_TIMEOUT and skips the
 * cgroup OOM check.) A blanket "close everything above 2" is not an option:
 * docs/SECURITY.md promises a forked child may keep using host-opened descriptors.
 * So each executor publishes exactly the ends IT owns and the child closes only
 * those. The lock is held across BOTH pipe creation and fork(), which is what
 * removes the window where a pipe exists but is not yet published. */
#define GPTPS_EXEC_MAX_FDS 256
static pthread_mutex_t g_execfd_m = PTHREAD_MUTEX_INITIALIZER;
static int             g_execfd[GPTPS_EXEC_MAX_FDS];
static unsigned        g_execfd_n = 0;

/* Move a pipe end above fds 0-2, still close-on-exec. Returns the descriptor to use
 * (`fd` itself when it is above 2 already), or -1 when no higher one is free. Runs
 * with g_execfd_m held and before the end is published, so no other executor's
 * fork() can catch the copy unregistered. */
static int fd_above_stdio(int fd)
{
    int n;
    if (fd > 2) return fd;
#if defined(F_DUPFD_CLOEXEC)
    n = fcntl(fd, F_DUPFD_CLOEXEC, 3);
#else
    n = fcntl(fd, F_DUPFD, 3);
    if (n >= 0) (void)fcntl(n, F_SETFD, fcntl(n, F_GETFD) | FD_CLOEXEC);
#endif
    if (n >= 0) close(fd);
    return n;
}

/* Create a pipe and publish both ends, atomically w.r.t. another executor's fork.
 * On overflow the end simply goes unpublished - degraded to the old behaviour for
 * that one descriptor rather than failing a task. With `high`, both ends are moved
 * above fd 2 first (gptps_oop_execute says why); a pipe that cannot be kept there
 * fails as a failed pipe() would. */
static int exec_pipe(int fds[2], int high)
{
    int rc;
    pthread_mutex_lock(&g_execfd_m);
    rc = make_pipe_cloexec(fds);
    if (rc == 0 && high) {
        int r0 = fd_above_stdio(fds[0]);
        int r1 = (r0 < 0) ? -1 : fd_above_stdio(fds[1]);
        if (r1 < 0) {                      /* no descriptor above 2 left */
            close(r0 < 0 ? fds[0] : r0);
            close(fds[1]);
            rc = -1;
        } else {
            fds[0] = r0; fds[1] = r1;
        }
    }
    if (rc == 0) {
        if (g_execfd_n < GPTPS_EXEC_MAX_FDS) g_execfd[g_execfd_n++] = fds[0];
        if (g_execfd_n < GPTPS_EXEC_MAX_FDS) g_execfd[g_execfd_n++] = fds[1];
    }
    pthread_mutex_unlock(&g_execfd_m);
    return rc;
}

/* Unpublish and close as ONE step: closing first would free the fd number while it
 * is still listed, and a concurrent thread could be handed it - after which the
 * next OOP child would close a descriptor that is not ours. */
static void exec_close(int fd)
{
    unsigned i;
    pthread_mutex_lock(&g_execfd_m);
    for (i = 0; i < g_execfd_n; ++i)
        if (g_execfd[i] == fd) { g_execfd[i] = g_execfd[--g_execfd_n]; break; }
    close(fd);
    pthread_mutex_unlock(&g_execfd_m);
}

/* fork() with the registry locked; in the CHILD, close every published descriptor
 * except this task's own `keep` ends, then drop the inherited list so a nested
 * engine opened in the child starts clean. Only close() runs before the unlock,
 * and unlocking a mutex this very thread holds is the standard pthread_atfork
 * child-handler discipline. Returns as fork() does. */
static pid_t exec_fork_sweep(const int *keep, unsigned nkeep)
{
    pid_t pid;
    pthread_mutex_lock(&g_execfd_m);
    pid = fork();
    if (pid == 0) {
        unsigned i, k;
        for (i = 0; i < g_execfd_n; ++i) {
            for (k = 0; k < nkeep; ++k) if (keep[k] == g_execfd[i]) break;
            if (k == nkeep) close(g_execfd[i]);
        }
        g_execfd_n = 0;
    }
    pthread_mutex_unlock(&g_execfd_m);
    return pid;
}

/* Coarse fallback cap: bound the child's virtual address space. Approximate
 * (caps VSZ not RSS; absent on macOS) but needs no privileges - used whenever
 * the accurate cgroup v2 path below is unavailable. */
static void apply_as_cap(uint64_t mem_cap)
{
#if defined(RLIMIT_AS)
    if (mem_cap >= GPTPS_OOP_MEMCAP_FLOOR) {
        struct rlimit rl; rl.rlim_cur = (rlim_t)mem_cap; rl.rlim_max = (rlim_t)mem_cap;
        setrlimit(RLIMIT_AS, &rl); /* best-effort */
    }
#else
    (void)mem_cap;
#endif
}

/* ---- accurate memory enforcement via cgroup v2 (Linux, opt-in) -------------
 * If GPTPS_CGROUP_PARENT names a cgroup whose subtree has the memory controller
 * delegated (e.g. a systemd Delegate=yes scope), each forked task gets its own
 * child cgroup with memory.max + memory.swap.max=0 - so exceeding the cap is an
 * actual RSS-based OOM-kill, not the coarse VSZ approximation RLIMIT_AS gives.
 * Every step is best-effort: any failure falls back to apply_as_cap(). */
#if defined(__linux__)
static unsigned cgroup_seq(void)
{
    static unsigned n = 0; /* unique suffix per concurrent task */
    return __atomic_add_fetch(&n, 1u, __ATOMIC_SEQ_CST);
}

static int cg_write_file(const char *dir, const char *file, const char *val)
{
    char path[512];   /* stack, not malloc: cgroup_self_join() calls this in the CHILD
                       * between fork and work/exec, where allocating in a threaded
                       * process can deadlock. Cgroup paths are short; bail if not. */
    int fd, n; ssize_t w;
    /* Bound via snprintf's return value rather than a separate strlen sum: same
     * guarantee, but it is the form the compiler can actually verify (the sum
     * version trips -Wformat-truncation at -O2, which blocks a -Werror build). */
    n = snprintf(path, sizeof path, "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= sizeof path) return -1;   /* would truncate: refuse */
    fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    w = write(fd, val, strlen(val));
    close(fd);
    return (w < 0) ? -1 : 0;
}

/* Create a child cgroup under $GPTPS_CGROUP_PARENT with the cap applied.
 * Returns a malloc'd path (caller frees + rmdirs) or NULL if unavailable. */
static char *cgroup_create(uint64_t mem_cap)
{
    const char *parent = getenv("GPTPS_CGROUP_PARENT");
    char val[32]; char *dir; size_t n;
    if (!parent || !*parent) return NULL;
    if (mem_cap < GPTPS_OOP_MEMCAP_FLOOR) return NULL;
    n = strlen(parent) + 48;
    dir = (char *)gptps_malloc(n);
    if (!dir) return NULL;
    snprintf(dir, n, "%s/gptps.%ld.%u", parent, (long)getpid(), cgroup_seq());
    if (mkdir(dir, 0700) != 0) { gptps_free(dir); return NULL; }
    snprintf(val, sizeof val, "%llu", (unsigned long long)mem_cap);
    if (cg_write_file(dir, "memory.max", val) != 0) { /* controller not delegated here */
        rmdir(dir); gptps_free(dir); return NULL;
    }
    cg_write_file(dir, "memory.swap.max", "0"); /* make the cap a hard ceiling, not swap */
    return dir;
}

/* Child moves itself into the cgroup (its first act, before allocating). */
static int cgroup_self_join(const char *dir)
{
    char pid[32];
    snprintf(pid, sizeof pid, "%ld", (long)getpid());
    return cg_write_file(dir, "cgroup.procs", pid);
}

/* Did the kernel OOM-kill anything in this cgroup? (memory.events: oom_kill N) */
static int cgroup_oom_killed(const char *dir)
{
    size_t n = strlen(dir) + 16;
    char *path = (char *)gptps_malloc(n), key[32];
    long v; int killed = 0;
    FILE *f;
    if (!path) return 0;
    snprintf(path, n, "%s/memory.events", dir);
    f = fopen(path, "r");
    gptps_free(path);
    if (!f) return 0;
    while (fscanf(f, "%31s %ld", key, &v) == 2)
        if (strcmp(key, "oom_kill") == 0 && v > 0) killed = 1;
    fclose(f);
    return killed;
}

static void cgroup_destroy(char *dir)
{
    if (dir) { rmdir(dir); gptps_free(dir); } /* child reaped => cgroup empty => rmdir succeeds */
}
#endif /* __linux__ */

/* ---- the child's report on how it started ---------------------------------
 * The child writes fixed-size records on a close-on-exec pipe of its own:
 *  - REPORT_STARTED, once it is set up and before it runs anything: whether it joined
 *    the job's cgroup. Only then do the cgroup's files describe the job. A child that
 *    could not join falls back to RLIMIT_AS and leaves the cgroup empty, and reading
 *    that would report zeros the job never had.
 *  - REPORT_EXEC, a program's, the last thing before exec: its resident high-water
 *    mark in KiB (Linux). fork() starts the copy's high-water mark at the host's own
 *    resident size, and exec() carries it into the ru_maxrss wait4 returns, so
 *    ru_maxrss is the program's own peak only where it is above this mark.
 * A program's pipe reaches EOF once exec has replaced the copy of the host, from when
 * a sample of its process describes the program. A record is 16 bytes, under
 * PIPE_BUF, so each write is whole and cannot block on a pipe this empty. A child
 * that cannot write one cannot talk to its parent and gives up, as it does when its
 * result cannot be written. */
#define REPORT_STARTED     1u
#define REPORT_EXEC        2u
#define GPTPS_HWM_UNKNOWN  UINT64_MAX
/* exec may fault in a few pages after the mark is read - execvp's own code and stack -
 * and they count toward the copy's mark too, so a peak this close above it is not
 * told apart from it. */
#define GPTPS_EXEC_HWM_SLACK_PAGES 64u
typedef struct { uint32_t stage; uint32_t joined; uint64_t hwm_kb; } child_report;

static int write_all(int fd, const void *buf, size_t n);

/* In the child: one record, or _exit. */
static void report_send(int fd, uint32_t stage, int joined, uint64_t hwm_kb)
{
    child_report r;
    memset(&r, 0, sizeof r);
    r.stage = stage; r.joined = joined ? 1u : 0u; r.hwm_kb = hwm_kb;
    if (write_all(fd, &r, sizeof r) != 0) _exit(127);
}

/* What the parent knows about the job it waits for. */
typedef struct {
    pid_t        pid;
    const char  *cgdir;      /* the job's cgroup in cgroup mode, else NULL */
    int          host_fork;  /* an OOP job: a copy of the host that never execs */
    int          fd;         /* the report pipe's read end, non-blocking; -1 after EOF */
    int          failed;     /* reading it failed: the attempt fails with GPTPS_E_IO */
    int          started, joined, at_exec;
    uint64_t     hwm_kb;
    size_t       got;
    child_report cur;
} job_watch;

/* Take what the child has reported so far, without blocking. */
static void report_poll(job_watch *jw)
{
    while (jw->fd >= 0) {
        ssize_t n = read(jw->fd, (char *)&jw->cur + jw->got, sizeof jw->cur - jw->got);
        if (n > 0) {
            jw->got += (size_t)n;
            if (jw->got < sizeof jw->cur) continue;
            jw->got = 0;
            if (jw->cur.stage == REPORT_STARTED)   { jw->started = 1; jw->joined = jw->cur.joined != 0; }
            else if (jw->cur.stage == REPORT_EXEC) { jw->at_exec = 1; jw->hwm_kb = jw->cur.hwm_kb; }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0) jw->failed = 1;
        exec_close(jw->fd);          /* EOF - every write end closed - or an error */
        jw->fd = -1;
    }
}

#if defined(__linux__)
/* The job's cgroup describes the job: there is one, and the child joined it. */
static int jw_cgroup(const job_watch *jw) { return jw->cgdir && jw->started && jw->joined; }
#endif

#if defined(__linux__) || defined(__APPLE__)
/* The job's process is the program now, not the copy of the host it began as. */
static int jw_exec_done(const job_watch *jw) { return jw->at_exec && jw->fd < 0 && !jw->failed; }
#endif

/* ---- measurements (docs/MEASUREMENTS.md) ----------------------------------
 * Every name and method below is a string literal, so the meter's entries stay valid
 * after the executor returns. gptps_meter_put keeps the first value per name, so a
 * caller puts the better method first and the fallback after it. */

#if defined(__linux__)
/* Read a small cgroup or /proc file into buf (NUL-terminated); its length, or -1. On
 * the stack, with open/read: these files are a few lines long. Close-on-exec, as the
 * executors' pipes are: another executor may fork and exec while this one reads. */
static long read_small(const char *dir, const char *file, char *buf, size_t cap)
{
    char path[512];
    int fd, n;
    long len = 0;
    n = snprintf(path, sizeof path, "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= sizeof path || cap == 0) return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    for (;;) {
        ssize_t r = read(fd, buf + len, cap - 1 - (size_t)len);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (r == 0 || (size_t)(len + r) >= cap - 1) { len += (r > 0 ? r : 0); break; }
        len += r;
    }
    close(fd);
    buf[len] = 0;
    return len;
}

/* The value after `key ` on a line of a "key value" file, e.g. memory.events. */
static int kv_find(const char *text, const char *key, uint64_t *out)
{
    size_t kl = strlen(key);
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == ' ') {
            *out = (uint64_t)strtoull(p + kl + 1, NULL, 10);
            return 1;
        }
        p = strchr(p, '\n');
        if (p) ++p;
    }
    return 0;
}

/* The job's cgroup: what every process in it used. Read after the child is reaped
 * and before the cgroup is removed, and only once the child said it joined (see the
 * child's report). A file the kernel does not have (memory.peak before 5.19) leaves
 * its names to the fallback. memory.events' `max` counts every time the job reached
 * its cap, page cache filling up to it included; `oom_kill` the times the kernel
 * killed for it. */
static void cgroup_measure(gptps_exec_meter *mt, const char *dir)
{
    char buf[4096];
    uint64_t v, w;
    if (!mt || !dir) return;
    if (read_small(dir, "memory.peak", buf, sizeof buf) > 0)
        gptps_meter_put(mt, GPTPS_M_MEM_PEAK, (uint64_t)strtoull(buf, NULL, 10),
                        GPTPS_UNIT_BYTES, GPTPS_MEASURE_PEAK, "cgroup.job.resident");
    if (read_small(dir, "memory.events", buf, sizeof buf) > 0 &&
        kv_find(buf, "max", &v) && kv_find(buf, "oom_kill", &w))
        gptps_meter_put(mt, GPTPS_M_MEM_CAP_HIT, (v > 0 || w > 0) ? 1u : 0u,
                        GPTPS_UNIT_FLAG, GPTPS_MEASURE_FLAG, "cgroup.job");
    if (read_small(dir, "cpu.stat", buf, sizeof buf) > 0 &&
        kv_find(buf, "user_usec", &v) && kv_find(buf, "system_usec", &w)) {
        gptps_meter_put(mt, GPTPS_M_CPU_USER_MS, v / 1000u, GPTPS_UNIT_MS, GPTPS_MEASURE_TOTAL, "cgroup.job");
        gptps_meter_put(mt, GPTPS_M_CPU_SYS_MS,  w / 1000u, GPTPS_UNIT_MS, GPTPS_MEASURE_TOTAL, "cgroup.job");
    }
}

/* io.stat: one line per device, "MAJ:MIN rbytes=N wbytes=N rios=N ...". It exists
 * only where the io controller is enabled for the job's cgroup, and an empty one means
 * no block I/O at all - a real 0. It counts bytes as they reach the device, so writes
 * still in the page cache when the job ends are not in it: the fallback to wait4's
 * figures, which count a write when it is made (meter_finish). */
static void cgroup_io_measure(gptps_exec_meter *mt, const char *dir)
{
    char buf[4096];
    if (!mt || !dir) return;
    if (read_small(dir, "io.stat", buf, sizeof buf) >= 0) {
        uint64_t rb = 0, wb = 0;
        const char *p = buf;
        while (*p) {
            const char *r = strstr(p, "rbytes="), *wr = strstr(p, "wbytes="), *nl = strchr(p, '\n');
            if (r && (!nl || r < nl))  rb += (uint64_t)strtoull(r + 7, NULL, 10);
            if (wr && (!nl || wr < nl)) wb += (uint64_t)strtoull(wr + 7, NULL, 10);
            if (!nl) break;
            p = nl + 1;
        }
        gptps_meter_put(mt, GPTPS_M_IO_READ_BYTES,  rb, GPTPS_UNIT_BYTES, GPTPS_MEASURE_TOTAL, "cgroup.job.block");
        gptps_meter_put(mt, GPTPS_M_IO_WRITE_BYTES, wb, GPTPS_UNIT_BYTES, GPTPS_MEASURE_TOTAL, "cgroup.job.block");
    }
}

/* ru_inblock / ru_oublock come from per-task I/O accounting, a kernel option: without
 * it they read 0 whatever the job did, so they are reported only where /proc/self/io
 * shows the accounting exists. 0 = not checked yet, 1 = absent, 2 = present. */
static uint32_t g_task_io = 0;
static int task_io_accounting(void)
{
    uint32_t v = gptps_hal_load_acquire_u32(&g_task_io);
    if (v == 0) {
        v = (access("/proc/self/io", R_OK) == 0) ? 2u : 1u;
        gptps_hal_store_release_u32(&g_task_io, v);
    }
    return v == 2u;
}

/* The value after `key` in a /proc/self file of "Key:   N kB" lines, or -1. */
static int64_t self_kb(const char *file, const char *key)
{
    char buf[4096];
    const char *p;
    if (read_small("/proc/self", file, buf, sizeof buf) <= 0) return -1;
    p = strstr(buf, key);
    return p ? (int64_t)strtoll(p + strlen(key), NULL, 10) : -1;
}

/* The most this process can bring to exec as its resident high-water mark, in KiB, or
 * GPTPS_HWM_UNKNOWN. Runs in the forked child: open and read, on the stack.
 * The mark exec takes is the larger of the one recorded at fork (VmHWM, which also
 * covers pages reclaimed since) and the resident size at exec. The kernel's resident
 * counters are per-CPU and approximate, so that size can sit above VmHWM as read a
 * moment earlier; it cannot sit above the exact resident size smaps_rollup counts,
 * except by the pages faulted in after this read (GPTPS_EXEC_HWM_SLACK_PAGES). */
static uint64_t self_hwm_kb(void)
{
    int64_t hwm = self_kb("status", "\nVmHWM:"), rss = self_kb("smaps_rollup", "\nRss:");
    if (hwm < 0 || rss < 0) return GPTPS_HWM_UNKNOWN;
    return (uint64_t)(hwm > rss ? hwm : rss);
}
#endif /* __linux__ */

#if GPTPS_HAVE_WAIT4
/* wait4's rusage: the job's process and the children it waited for. ru_maxrss is the
 * resident peak of the largest of those processes, not their sum: in KiB on Linux and
 * the BSDs, in bytes on macOS.
 *  - An OOP job's process is a fork of the host and its resident set holds host pages
 *    (on Linux all the fork copied, from the start), so it has its own method.
 *  - On Linux a program's ru_maxrss also covers the copy of the host it was forked
 *    as, before exec: it is the program's own only above that copy's high-water mark
 *    (the child's report), and absent otherwise. */
static void rusage_measure(gptps_exec_meter *mt, const struct rusage *ru, const job_watch *jw)
{
    uint64_t rss;
    int own = 1;
    if (!mt) return;
#if defined(__APPLE__)
    rss = (uint64_t)ru->ru_maxrss;
#else
    rss = (uint64_t)ru->ru_maxrss * 1024u;
#endif
#if defined(__linux__)
    if (!jw->host_fork) {
        uint64_t slack_kb = (uint64_t)GPTPS_EXEC_HWM_SLACK_PAGES * (uint64_t)sysconf(_SC_PAGESIZE) / 1024u;
        own = jw->at_exec && jw->hwm_kb != GPTPS_HWM_UNKNOWN &&
              (uint64_t)ru->ru_maxrss > jw->hwm_kb + slack_kb;
    }
#elif !defined(__APPLE__)
    own = 0;   /* macOS starts a program's count afresh at exec (test_measure checks it);
                * on the BSDs that is not checked, so it is not reported (rule 2) */
#endif
    if (jw->host_fork)
        gptps_meter_put(mt, GPTPS_M_MEM_PEAK, rss, GPTPS_UNIT_BYTES, GPTPS_MEASURE_PEAK,
                        "rusage.host_fork.resident");
    else if (own)
        gptps_meter_put(mt, GPTPS_M_MEM_PEAK, rss, GPTPS_UNIT_BYTES, GPTPS_MEASURE_PEAK,
                        "rusage.largest_process.resident");
    gptps_meter_put(mt, GPTPS_M_CPU_USER_MS,
                    (uint64_t)ru->ru_utime.tv_sec * 1000u + (uint64_t)ru->ru_utime.tv_usec / 1000u,
                    GPTPS_UNIT_MS, GPTPS_MEASURE_TOTAL, "rusage.process");
    gptps_meter_put(mt, GPTPS_M_CPU_SYS_MS,
                    (uint64_t)ru->ru_stime.tv_sec * 1000u + (uint64_t)ru->ru_stime.tv_usec / 1000u,
                    GPTPS_UNIT_MS, GPTPS_MEASURE_TOTAL, "rusage.process");
#if defined(__linux__)
    /* Linux counts these in 512-byte units of block-device I/O; elsewhere they are
     * counts of operations, which are not bytes, so they are not reported. */
    if (task_io_accounting()) {
        gptps_meter_put(mt, GPTPS_M_IO_READ_BYTES, (uint64_t)ru->ru_inblock * 512u,
                        GPTPS_UNIT_BYTES, GPTPS_MEASURE_TOTAL, "rusage.process.block");
        gptps_meter_put(mt, GPTPS_M_IO_WRITE_BYTES, (uint64_t)ru->ru_oublock * 512u,
                        GPTPS_UNIT_BYTES, GPTPS_MEASURE_TOTAL, "rusage.process.block");
    }
#endif
}
#endif

/* Everything an ended attempt reports, best method first: the cgroup's memory, cap
 * and CPU (the whole job), then wait4's (the process), then the cgroup's I/O as the
 * fallback for wait4's, which counts writes when they are made. */
static void meter_finish(gptps_exec_meter *mt, const job_watch *jw, const struct rusage *ru, int have_ru)
{
    if (!mt) return;
#if defined(__linux__)
    if (jw_cgroup(jw)) cgroup_measure(mt, jw->cgdir);
#endif
#if GPTPS_HAVE_WAIT4
    if (have_ru) rusage_measure(mt, ru, jw);
#else
    (void)ru; (void)have_ru;
#endif
#if defined(__linux__)
    if (jw_cgroup(jw)) cgroup_io_measure(mt, jw->cgdir);
#else
    (void)jw;
#endif
}

/* Samples of a running job, about every sample_ms (at least 10), driven by the
 * executor's own wait loop: sampler_slice shortens a wait to the next sample, and
 * sampler_tick takes it when it is due. */
typedef struct { gptps_exec_meter *mt; uint64_t next; uint32_t iv; } sampler;

static void sampler_init(sampler *sm, gptps_exec_meter *mt)
{
    sm->mt = (mt && mt->sample_ms && mt->sample) ? mt : NULL;
    sm->iv = sm->mt ? (mt->sample_ms < 10u ? 10u : mt->sample_ms) : 0;
    sm->next = sm->mt ? gptps_hal_monotonic_ms() + sm->iv : 0;
}

static int sampler_slice(const sampler *sm, int slice)
{
    uint64_t now;
    if (!sm->mt) return slice;
    now = gptps_hal_monotonic_ms();
    if (now >= sm->next) return 0;
    return (sm->next - now < (uint64_t)slice) ? (int)(sm->next - now) : slice;
}

/* The job's memory now: its cgroup's on Linux in cgroup mode, else its main
 * process's - a program's only once it has exec'd, an OOP job's under its own method
 * (rusage_measure says why). Nothing where none of that can be read yet. */
static void sampler_tick(sampler *sm, job_watch *jw)
{
    gptps_measure cur[1];
    size_t n = 0;
    uint64_t now;
    if (!sm->mt) return;
    now = gptps_hal_monotonic_ms();
    if (now < sm->next) return;
    sm->next = now + sm->iv;
    report_poll(jw);
#if defined(__linux__)
    {
        char buf[256];
        if (jw_cgroup(jw)) {
            if (read_small(jw->cgdir, "memory.current", buf, sizeof buf) > 0) {
                cur[0].value = (uint64_t)strtoull(buf, NULL, 10);
                cur[0].method = "cgroup.job.resident";
                n = 1;
            }
        } else if (jw->cgdir && !jw->started && jw->fd >= 0) {
            /* not known yet whether the child is in its cgroup: no sample this time */
        } else if (jw->host_fork || jw_exec_done(jw)) {
            char dir[64], *end;
            long size, pages;
            snprintf(dir, sizeof dir, "/proc/%ld", (long)jw->pid);
            /* statm: "size resident shared ...", in pages. A process that has exited
             * and not been reaped yet reads all zeros: it has no memory left to
             * sample, so a size of 0 is no sample rather than a sample of 0. */
            if (read_small(dir, "statm", buf, sizeof buf) > 0) {
                size = strtol(buf, &end, 10);
                pages = (end != buf) ? strtol(end, NULL, 10) : -1;
                if (size > 0 && pages >= 0) {
                    cur[0].value = (uint64_t)pages * (uint64_t)sysconf(_SC_PAGESIZE);
                    cur[0].method = jw->host_fork ? "procfs.host_fork.resident" : "procfs.process.resident";
                    n = 1;
                }
            }
        }
    }
#elif defined(__APPLE__)
    {
        struct proc_taskinfo ti;
        if ((jw->host_fork || jw_exec_done(jw)) &&
            proc_pidinfo(jw->pid, PROC_PIDTASKINFO, 0, &ti, (int)sizeof ti) == (int)sizeof ti) {
            cur[0].value = (uint64_t)ti.pti_resident_size;
            cur[0].method = jw->host_fork ? "libproc.host_fork.resident" : "libproc.process.resident";
            n = 1;
        }
    }
#endif
    if (n) {
        cur[0].name = GPTPS_M_MEM_CURRENT;
        cur[0].unit = GPTPS_UNIT_BYTES; cur[0].kind = GPTPS_MEASURE_CURRENT; cur[0].flags = 0;
        sm->mt->sample(sm->mt, cur, n);
    }
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf; size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf; size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1; /* EOF before full record => child died early */
        off += (size_t)r;
    }
    return 0;
}

gptps_status gptps_oop_execute(const gptps_task_def *def, const void *payload, size_t plen,
                               uint64_t mem_cap, uint32_t timeout_s, const uint32_t *cancel,
                               void **out_result, size_t *out_len, gptps_exec_meter *meter)
{
    int p[2], rp[2];
    pid_t pid;
#if defined(__linux__)
    char *cgdir = cgroup_create(mem_cap);
#endif

    *out_result = NULL; *out_len = 0;
    /* Both ends above fd 2. A host that daemonised - closed its stdin and stdout, a
     * standard step - gets them back from pipe() as fds 0 and 1, and the child's end
     * was then the task's own stdout: whatever the task printed went into the result
     * record ahead of it, the parent read that text as the record's header, and the
     * task failed with GPTPS_E_IO. (The PROGRAM child moves its own ends off 0-2,
     * before its dup2s.) */
    if (exec_pipe(p, 1) != 0) {
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }
    if (exec_pipe(rp, 1) != 0) {   /* the child's report: above fd 2 for the same reason */
        exec_close(p[0]); exec_close(p[1]);
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }

    {
        int keep[4];
        keep[0] = p[0]; keep[1] = p[1]; keep[2] = rp[0]; keep[3] = rp[1];
        pid = exec_fork_sweep(keep, 4);   /* keep our own ends; shed every other executor's */
    }
    if (pid < 0) {
        exec_close(p[0]); exec_close(p[1]); exec_close(rp[0]); exec_close(rp[1]);
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }

    if (pid == 0) {
        /* ---- CHILD ---- */
        void *res = NULL; size_t rlen = 0;
        int32_t st32; uint64_t len64;
        gptps_status st;
        int joined = 0;
        signal(SIGPIPE, SIG_IGN);
        close(p[0]); close(rp[0]);
#if defined(__linux__)
        if (cgdir && cgroup_self_join(cgdir) == 0) joined = 1; /* accurate RSS cap */
#endif
        if (!joined) apply_as_cap(mem_cap);                    /* coarse fallback */
        report_send(rp[1], REPORT_STARTED, joined, GPTPS_HWM_UNKNOWN);
        close(rp[1]);                                          /* nothing more to report */
        if (def->child_setup) def->child_setup(def->user_data); /* host hardening hook */
        st = gptps_run_capture(def, payload, plen, &res, &rlen);
        st32 = (int32_t)st;
        /* Never let a child declare more than the parent will accept - the parent
         * rejects an oversize record, so sending it would just desynchronise. */
        len64 = (rlen > GPTPS_EXEC_RESULT_CAP) ? 0 : (uint64_t)rlen;
        if (rlen > GPTPS_EXEC_RESULT_CAP) st32 = (int32_t)GPTPS_E_IO;
        /* Stop at the first write that fails. Nothing makes the write after a failed
         * one fail too - a pipe write fails with ENOMEM when no page can be had for
         * the buffer, and the next may find one - and the record has nothing to
         * resync on: had the length gone missing and the payload still followed, the
         * parent would read the payload's first 8 bytes AS the length, and a payload
         * that starts with a small number parses as a whole record - a FINISHED with
         * the wrong bytes. Cut short, it is a torn record the parent rejects. */
        if (write_all(p[1], &st32, sizeof st32) == 0 &&
            write_all(p[1], &len64, sizeof len64) == 0 && len64)
            (void)write_all(p[1], res, (size_t)len64);
        /* Deliberately no free() and no allocator call on this path: we are in a
         * forked child of a threaded process, so a host allocator installed via
         * gptps_set_allocator may hold a mutex locked by a thread that did not
         * survive the fork. _exit() reclaims everything. */
        close(p[1]);
        _exit(0);
    }

    /* ---- PARENT ---- */
    {
        struct pollfd pfd;
        int killed = 0, pr, wstatus = 0, reaped = 0;
        int32_t st32 = (int32_t)GPTPS_E_TASK;
        uint64_t len64 = 0;
        void *res = NULL;
        gptps_status eff;
        gptps_status kill_st = GPTPS_E_TIMEOUT;   /* why we killed the child, if we did */
        uint64_t deadline = timeout_s ? gptps_hal_monotonic_ms() + (uint64_t)timeout_s * 1000u : 0;
        struct rusage ru;                          /* the child's, from wait4 */
        int have_ru = 0;
        sampler sm;
        job_watch jw;
        memset(&jw, 0, sizeof jw);
        jw.pid = pid; jw.host_fork = 1; jw.fd = rp[0];
#if defined(__linux__)
        jw.cgdir = cgdir;
#endif
        memset(&ru, 0, sizeof ru);
        sampler_init(&sm, meter);

        exec_close(p[1]); exec_close(rp[1]);
        (void)fcntl(rp[0], F_SETFL, fcntl(rp[0], F_GETFL) | O_NONBLOCK);
        pfd.fd = p[0]; pfd.events = POLLIN; pfd.revents = 0;

        /* Wait for the child's result in bounded slices so a raised cancel flag (a
         * gptps_cancel / shutdown / task removal) OR the deadline hard-kills the child
         * instead of blocking this worker forever - including when timeout_s==0. */
        for (;;) {
            int slice = 200;
            sampler_tick(&sm, &jw);                /* a sample, when one is due */
            if (deadline) {
                uint64_t now = gptps_hal_monotonic_ms();
                if (now >= deadline) { kill(pid, SIGKILL); killed = 1; kill_st = GPTPS_E_TIMEOUT; break; }
                if (deadline - now < (uint64_t)slice) slice = (int)(deadline - now);
            }
            slice = sampler_slice(&sm, slice);
            pr = poll(&pfd, 1, slice);
            /* An explicit gptps_cancel / shutdown / task removal is NOT a deadline
             * breach - report the two apart so an operator can tell which happened. */
            if (cancel && gptps_hal_load_acquire_u32(cancel)) { kill(pid, SIGKILL); killed = 1; kill_st = GPTPS_E_CANCELLED; break; }
            /* a genuine poll error must kill (killed=1 skips the blocking read_all below,
             * which would otherwise hang this worker on a still-live child). */
            if (pr < 0) { if (errno == EINTR) continue; kill(pid, SIGKILL); killed = 1; kill_st = GPTPS_E_IO; break; }
            if (pr == 0) continue;                            /* slice elapsed: re-check */
            break;                                            /* readable: read the record */
        }

        if (!killed) {
            if (read_all(p[0], &st32, sizeof st32) == 0 &&
                read_all(p[0], &len64, sizeof len64) == 0) {
                if (len64 > (uint64_t)GPTPS_EXEC_RESULT_CAP) {
                    /* Torn or corrupted record. Do not allocate what it asks for and
                     * do not trust the rest of the stream (on 32-bit, (size_t)len64
                     * would truncate and desynchronise the read). */
                    len64 = 0; st32 = (int32_t)GPTPS_E_IO;
                } else if (len64) {
                    res = gptps_malloc((size_t)len64);
                    if (!res || read_all(p[0], res, (size_t)len64) != 0) {
                        gptps_free(res); res = NULL; len64 = 0; st32 = (int32_t)GPTPS_E_IO;
                    }
                }
            } else {
                st32 = (int32_t)GPTPS_E_TASK; /* child died before writing a record */
            }
        }
        exec_close(p[0]);
        if (killed) {
            while (wait_child(pid, &wstatus, 0, &ru, &have_ru) < 0 && errno == EINTR) { /* SIGKILLed: bounded */ }
        } else {
            /* The child owes us nothing more, but it has not necessarily exited. */
            gptps_status why = reap_bounded(pid, &wstatus, &reaped, 0, deadline, cancel, &ru, &have_ru);
            if (why != GPTPS_OK) { killed = 1; kill_st = why; }
        }

        if (killed) {
            eff = kill_st;
        } else if (reaped && WIFSIGNALED(wstatus)) {
            /* `reaped` gate: with an unreapable child wstatus is untouched, so the
             * signal-death inference would be a guess. Fall through to st32, which
             * the child reported over the pipe and is already authoritative. */
            gptps_free(res); res = NULL; len64 = 0;   /* crash / OOM-kill */
            eff = GPTPS_E_TASK;
#if defined(__linux__)
            if (cgdir && cgroup_oom_killed(cgdir)) eff = GPTPS_E_NOMEM; /* exceeded the memory cap */
#endif
        } else {
            eff = (gptps_status)st32;
        }
        report_poll(&jw);                          /* the child is gone: all of it */
        if (jw.fd >= 0) exec_close(jw.fd);         /* a write end lives on elsewhere */
        if (jw.failed && eff == GPTPS_OK) {        /* our own pipe failed us */
            gptps_free(res); res = NULL; len64 = 0; eff = GPTPS_E_IO;
        }
        meter_finish(meter, &jw, &ru, have_ru);   /* before the cgroup goes */
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        *out_result = res; *out_len = (size_t)len64;
        return eff;
    }
}

#define GPTPS_PROG_RESULT_CAP GPTPS_EXEC_RESULT_CAP /* max captured stdout */

gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s, const uint32_t *cancel,
                                   void **out_result, size_t *out_len, gptps_exec_meter *meter)
{
    const char *const *argv = def ? def->argv : NULL;
    int inp[2], outp[2], rp[2];
    pid_t pid;
#if defined(__linux__)
    char *cgdir = cgroup_create(mem_cap);
#endif

    *out_result = NULL; *out_len = 0;
    if (!argv || !argv[0]) {
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_INVAL;
    }
    if (exec_pipe(inp, 0) != 0) {
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }
    if (exec_pipe(outp, 0) != 0) {
        exec_close(inp[0]); exec_close(inp[1]);
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }
    /* The child's report, above fd 2: its ends are not among those the child hoists
     * before its dup2s, and close-on-exec takes the write end away at exec. */
    if (exec_pipe(rp, 1) != 0) {
        exec_close(inp[0]); exec_close(inp[1]); exec_close(outp[0]); exec_close(outp[1]);
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }

    {
        int keep[6];
        keep[0] = inp[0]; keep[1] = inp[1]; keep[2] = outp[0]; keep[3] = outp[1];
        keep[4] = rp[0]; keep[5] = rp[1];
        pid = exec_fork_sweep(keep, 6);
    }
    if (pid < 0) {
        exec_close(inp[0]); exec_close(inp[1]); exec_close(outp[0]); exec_close(outp[1]);
        exec_close(rp[0]); exec_close(rp[1]);
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }

    if (pid == 0) {
        /* ---- CHILD: wire stdin/stdout to the pipes, cap memory, exec ---- */
        int joined = 0;
        int i, *ends[4];
        /* Hoist EVERY pipe end above fd 2 first. A host that daemonised (closed its
         * stdin, a standard step) leaves fd 0 free, so pipe() hands back inp[0]==0:
         * dup2(inp[0], 0) is then a no-op that does not even clear FD_CLOEXEC, and
         * the close() below shuts fd 0 outright - the program execs with no stdin
         * and the payload is silently dropped. With 0 and 1 both free it is worse:
         * inp=={0,1}, so close(inp[1]) closes the stdout we just dup2'd onto fd 1.
         * Any of the four ends can alias, hence all four; closing the low original
         * keeps it from aliasing a dup2 target below. */
        ends[0] = &inp[0]; ends[1] = &inp[1]; ends[2] = &outp[0]; ends[3] = &outp[1];
        for (i = 0; i < 4; ++i) {
            if (*ends[i] < 3) {
                int n = fcntl(*ends[i], F_DUPFD, 3);  /* the copy is closed below, so
                                                       * plain F_DUPFD is enough and
                                                       * stays POSIX.1-2001-portable */
                if (n < 0) _exit(127);
                close(*ends[i]);
                *ends[i] = n;
            }
        }
        if (dup2(inp[0], STDIN_FILENO) < 0 || dup2(outp[1], STDOUT_FILENO) < 0) _exit(127);
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]); close(rp[0]);
        setpgid(0, 0); /* own process group: a timeout kill takes down the program AND its children */
#if defined(__linux__)
        if (cgdir && cgroup_self_join(cgdir) == 0) joined = 1; /* accurate RSS cap before exec */
#endif
        if (!joined) apply_as_cap(mem_cap);                    /* coarse fallback */
        report_send(rp[1], REPORT_STARTED, joined, GPTPS_HWM_UNKNOWN);
        if (def->child_setup) def->child_setup(def->user_data); /* host hardening hook (chdir/seccomp/drop-privs/...) */
#if defined(__linux__)
        report_send(rp[1], REPORT_EXEC, joined, self_hwm_kb()); /* the last thing before exec */
#else
        report_send(rp[1], REPORT_EXEC, joined, GPTPS_HWM_UNKNOWN);
#endif
        execvp(argv[0], (char *const *)argv); /* PATH-resolves a bare name (e.g. "wasmtime") */
        _exit(127); /* exec failed */
    }

    /* ---- PARENT: pump payload->stdin and stdout->buf CONCURRENTLY, up to the
     * deadline / until cancelled. A single poll loop over both pipes is what avoids
     * the deadlock the old "write all of stdin, THEN read stdout" had: a streaming
     * child that emits output while still reading a large input would fill its stdout
     * pipe (parent not yet reading) while the parent blocked filling its stdin pipe. */
    {
        gptps_status eff = GPTPS_OK;
        int killed = 0, oversize = 0, nomem = 0, wstatus = 0, reaped = 0;
        gptps_status kill_st = GPTPS_E_TIMEOUT;   /* why we killed the child, if we did */
        char *buf = NULL; size_t cap = 0, len = 0;
        const char *wp = (const char *)payload;   /* unwritten payload cursor */
        size_t wleft = plen;
        int in_open = 1;                           /* inp[1] still open for writing */
        uint64_t deadline = timeout_s ? gptps_hal_monotonic_ms() + (uint64_t)timeout_s * 1000u : 0;
        struct rusage ru;                          /* the child's, from wait4 */
        int have_ru = 0;
        sampler sm;
        job_watch jw;
#if !defined(F_SETNOSIGPIPE)
        sigset_t sp_old; int sp_masked = 0;
#endif
        memset(&jw, 0, sizeof jw);
        jw.pid = pid; jw.fd = rp[0];
#if defined(__linux__)
        jw.cgdir = cgdir;
#endif
        memset(&ru, 0, sizeof ru);
        sampler_init(&sm, meter);

        exec_close(inp[0]); exec_close(outp[1]); exec_close(rp[1]);
        (void)fcntl(rp[0], F_SETFL, fcntl(rp[0], F_GETFL) | O_NONBLOCK);
        setpgid(pid, pid);                         /* idempotent with the child: race-free group setup */
        /* A write to the child's closed stdin must EPIPE, not kill us - but suppress
         * SIGPIPE WITHOUT mutating the host's process-wide disposition: per-fd on
         * macOS/BSD (F_SETNOSIGPIPE), else per-THREAD (block SIGPIPE now; drain any it
         * leaves pending and restore after the pump - see below). */
#if defined(F_SETNOSIGPIPE)
        (void)fcntl(inp[1], F_SETNOSIGPIPE, 1);
#else
        {
            sigset_t sp; sigemptyset(&sp); sigaddset(&sp, SIGPIPE);
            sp_masked = (pthread_sigmask(SIG_BLOCK, &sp, &sp_old) == 0);
        }
#endif
        /* non-blocking stdin so a POLLOUT-guarded write never stalls the read side */
        (void)fcntl(inp[1], F_SETFL, fcntl(inp[1], F_GETFL) | O_NONBLOCK);
        if (!wleft) { exec_close(inp[1]); in_open = 0; } /* no payload: EOF the child's stdin now */

        for (;;) {
            struct pollfd pfd[2]; int nfd = 0, oidx, iidx = -1, pr; int slice = 200;

            oidx = nfd; pfd[nfd].fd = outp[0]; pfd[nfd].events = POLLIN;  pfd[nfd].revents = 0; nfd++;
            if (in_open) { iidx = nfd; pfd[nfd].fd = inp[1]; pfd[nfd].events = POLLOUT; pfd[nfd].revents = 0; nfd++; }

            /* Every pass, not only on an idle slice: a program that writes all the
             * time would otherwise never be sampled. */
            sampler_tick(&sm, &jw);
            if (deadline) {
                uint64_t now = gptps_hal_monotonic_ms();
                if (now >= deadline) { killed = 1; kill_st = GPTPS_E_TIMEOUT; kill(-pid, SIGKILL); break; }
                if (deadline - now < (uint64_t)slice) slice = (int)(deadline - now);
            }
            slice = sampler_slice(&sm, slice);
            pr = poll(pfd, (nfds_t)nfd, slice);
            /* An explicit gptps_cancel / shutdown / task removal is NOT a deadline
             * breach - report the two apart so an operator can tell which happened. */
            if (cancel && gptps_hal_load_acquire_u32(cancel)) { killed = 1; kill_st = GPTPS_E_CANCELLED; kill(-pid, SIGKILL); break; }
            /* a genuine poll error (e.g. ENOMEM) must still kill the child, or the
             * blocking waitpid below would hang this worker on a still-live child -
             * the very thing this executor promises never to do. */
            if (pr < 0) { if (errno == EINTR) continue; killed = 1; kill_st = GPTPS_E_IO; kill(-pid, SIGKILL); break; }
            if (pr == 0) continue;                 /* slice elapsed: re-check deadline/cancel */

            /* drain stdout */
            if (pfd[oidx].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r;
                char probe;                        /* where a byte past the cap lands */
                int full = 0;                      /* the buffer is at the cap */
                if (len == cap) {
                    size_t ncap = cap ? cap * 2 : 65536;
                    char *nb;
                    if (ncap > GPTPS_PROG_RESULT_CAP) ncap = GPTPS_PROG_RESULT_CAP;
                    full = (ncap == cap);
                    if (!full) {
                        nb = (char *)gptps_realloc(buf, ncap);
                        if (!nb) { nomem = 1; kill(-pid, SIGKILL); break; }
                        buf = nb; cap = ncap;
                    }
                }
                /* At the cap, read one byte into `probe`: EOF there is a result of
                 * exactly the cap, which is allowed (the OOP executor takes one too),
                 * and only a byte past it is oversize. Refusing as soon as the buffer
                 * filled turned a result of exactly 16 MiB into GPTPS_E_IO. */
                r = full ? read(outp[0], &probe, 1) : read(outp[0], buf + len, cap - len);
                if (r > 0 && full) { oversize = 1; kill(-pid, SIGKILL); break; } /* >16 MiB */
                else if (r > 0) len += (size_t)r;
                else if (r == 0) break;            /* stdout EOF: child is done */
                else if (errno != EINTR && errno != EAGAIN) { killed = 1; kill_st = GPTPS_E_IO; kill(-pid, SIGKILL); break; } /* I/O error: kill so waitpid can't hang */
            }
            /* feed stdin */
            if (in_open && (pfd[iidx].revents & (POLLOUT | POLLERR | POLLHUP))) {
                if (pfd[iidx].revents & (POLLERR | POLLHUP)) {   /* child closed its stdin */
                    exec_close(inp[1]); in_open = 0;
                } else {
                    ssize_t w = write(inp[1], wp, wleft);
                    if (w > 0) { wp += w; wleft -= (size_t)w; if (!wleft) { exec_close(inp[1]); in_open = 0; } }
                    else if (w < 0 && errno == EPIPE) { exec_close(inp[1]); in_open = 0; } /* child closed its stdin */
                    /* Any other error (ENOMEM: no page for the pipe buffer) means the payload
                     * cannot be delivered. Closing stdin as for EPIPE would hand the program
                     * a truncated payload as if it were all of it, and a program that exits 0
                     * on that is a FINISHED with the wrong result. Fail it, as a read error does. */
                    else if (w < 0 && errno != EINTR && errno != EAGAIN) { killed = 1; kill_st = GPTPS_E_IO; kill(-pid, SIGKILL); break; }
                }
            }
        }
        if (in_open) exec_close(inp[1]);
        exec_close(outp[0]);
#if !defined(F_SETNOSIGPIPE)
        if (sp_masked) {
            /* consume a SIGPIPE our blocked writes may have left pending (else it
             * would fire on restore), then restore this thread's original mask. */
            struct timespec z; sigset_t sp;
            z.tv_sec = 0; z.tv_nsec = 0;
            sigemptyset(&sp); sigaddset(&sp, SIGPIPE);
            while (sigtimedwait(&sp, NULL, &z) >= 0) { }
            pthread_sigmask(SIG_SETMASK, &sp_old, NULL);
        }
#endif
        if (killed || oversize || nomem) {
            while (wait_child(pid, &wstatus, 0, &ru, &have_ru) < 0 && errno == EINTR) { /* SIGKILLed: bounded */ }
        } else {
            /* Loop exited on stdout EOF - which says the child closed its stdout, NOT
             * that it exited. A program that keeps running (or ignores signals) would
             * otherwise pin this worker in waitpid() forever, orphaning the child at
             * shutdown. Give it a bounded grace period, then SIGKILL. */
            gptps_status why = reap_bounded(pid, &wstatus, &reaped, 1, deadline, cancel, &ru, &have_ru);
            if (why != GPTPS_OK) { killed = 1; kill_st = why; }
        }

        if (killed)                  eff = kill_st;
        else if (oversize)           eff = GPTPS_E_IO;
        else if (nomem)              eff = GPTPS_E_NOMEM;
        /* Someone else reaped the child (host ignores SIGCHLD, or runs a wait-any
         * reaper), so wstatus is untouched and WIFEXITED(0)/WEXITSTATUS(0) would
         * report every FAILING program as a success. An unknowable exit status is
         * a task error - that keeps retries, failure policy and dead-lettering
         * working instead of silently inverting the "non-zero => E_TASK" contract. */
        else if (!reaped)            eff = GPTPS_E_TASK;
        else if (WIFEXITED(wstatus)) eff = (WEXITSTATUS(wstatus) == 0) ? GPTPS_OK : GPTPS_E_TASK;
        else                         eff = GPTPS_E_TASK; /* killed by a signal */
#if defined(__linux__)
        if (cgdir && eff != GPTPS_OK && eff != GPTPS_E_TIMEOUT && cgroup_oom_killed(cgdir))
            eff = GPTPS_E_NOMEM;     /* exceeded the memory cap */
#endif
        report_poll(&jw);                          /* the child is gone: all of it */
        if (jw.fd >= 0) exec_close(jw.fd);         /* a write end lives on elsewhere */
        if (jw.failed && eff == GPTPS_OK) eff = GPTPS_E_IO;   /* our own pipe failed us */
        meter_finish(meter, &jw, &ru, have_ru);   /* before the cgroup goes */
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif

        if (eff == GPTPS_OK) { *out_result = buf; *out_len = len; }
        else gptps_free(buf);
        return eff;
    }
}

#endif /* !_WIN32 */
