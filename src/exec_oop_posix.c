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
 *  - timeout_s==0 => no deadline; a task that never returns blocks its worker.
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

#define GPTPS_OOP_MEMCAP_FLOOR (16ull * 1024ull * 1024ull) /* below this, a mem cap is meaningless */

/* Create a pipe with both ends close-on-exec so a concurrently-forked child
 * (especially a PROGRAM child that exec()s) cannot inherit and pin open another
 * executor's pipe ends - which would hang that executor's parent waiting for EOF. */
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
    int fd; ssize_t w;
    if (strlen(dir) + 1 + strlen(file) + 1 > sizeof path) return -1;
    snprintf(path, sizeof path, "%s/%s", dir, file);
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
                               uint64_t mem_cap, uint32_t timeout_s, gptps_flag *cancel,
                               void **out_result, size_t *out_len)
{
    int p[2];
    pid_t pid;
#if defined(__linux__)
    char *cgdir = cgroup_create(mem_cap);
#endif

    *out_result = NULL; *out_len = 0;
    if (make_pipe_cloexec(p) != 0) {
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }

    pid = fork();
    if (pid < 0) {
        close(p[0]); close(p[1]);
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
        close(p[0]);
#if defined(__linux__)
        if (cgdir && cgroup_self_join(cgdir) == 0) joined = 1; /* accurate RSS cap */
#endif
        if (!joined) apply_as_cap(mem_cap);                    /* coarse fallback */
        if (def->child_setup) def->child_setup(def->user_data); /* host hardening hook */
        st = gptps_run_capture(def, payload, plen, &res, &rlen);
        st32 = (int32_t)st; len64 = (uint64_t)rlen;
        write_all(p[1], &st32, sizeof st32);
        write_all(p[1], &len64, sizeof len64);
        if (rlen) write_all(p[1], res, rlen);
        gptps_free(res);
        close(p[1]);
        _exit(0);
    }

    /* ---- PARENT ---- */
    {
        struct pollfd pfd;
        int killed = 0, pr, wstatus = 0;
        int32_t st32 = (int32_t)GPTPS_E_TASK;
        uint64_t len64 = 0;
        void *res = NULL;
        gptps_status eff;
        uint64_t deadline = timeout_s ? gptps_hal_monotonic_ms() + (uint64_t)timeout_s * 1000u : 0;

        close(p[1]);
        pfd.fd = p[0]; pfd.events = POLLIN; pfd.revents = 0;

        /* Wait for the child's result in bounded slices so a raised cancel flag (a
         * gptps_cancel / shutdown / task removal) OR the deadline hard-kills the child
         * instead of blocking this worker forever - including when timeout_s==0. */
        for (;;) {
            int slice = 200;
            if (deadline) {
                uint64_t now = gptps_hal_monotonic_ms();
                if (now >= deadline) { kill(pid, SIGKILL); killed = 1; break; }    /* deadline */
                if (deadline - now < (uint64_t)slice) slice = (int)(deadline - now);
            }
            pr = poll(&pfd, 1, slice);
            if (cancel && gptps_flag_get(cancel)) { kill(pid, SIGKILL); killed = 1; break; } /* cancelled */
            /* a genuine poll error must kill (killed=1 skips the blocking read_all below,
             * which would otherwise hang this worker on a still-live child). */
            if (pr < 0) { if (errno == EINTR) continue; kill(pid, SIGKILL); killed = 1; break; }
            if (pr == 0) continue;                            /* slice elapsed: re-check */
            break;                                            /* readable: read the record */
        }

        if (!killed) {
            if (read_all(p[0], &st32, sizeof st32) == 0 &&
                read_all(p[0], &len64, sizeof len64) == 0) {
                if (len64) {
                    res = gptps_malloc((size_t)len64);
                    if (!res || read_all(p[0], res, (size_t)len64) != 0) {
                        gptps_free(res); res = NULL; len64 = 0; st32 = (int32_t)GPTPS_E_IO;
                    }
                }
            } else {
                st32 = (int32_t)GPTPS_E_TASK; /* child died before writing a record */
            }
        }
        close(p[0]);
        while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) { /* reap */ }

        if (killed) {
            eff = GPTPS_E_TIMEOUT;
        } else if (WIFSIGNALED(wstatus)) {
            gptps_free(res); res = NULL; len64 = 0;   /* crash / OOM-kill */
            eff = GPTPS_E_TASK;
#if defined(__linux__)
            if (cgdir && cgroup_oom_killed(cgdir)) eff = GPTPS_E_NOMEM; /* exceeded the memory cap */
#endif
        } else {
            eff = (gptps_status)st32;
        }
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        *out_result = res; *out_len = (size_t)len64;
        return eff;
    }
}

#define GPTPS_PROG_RESULT_CAP (16u * 1024u * 1024u) /* max captured stdout */

gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s, gptps_flag *cancel,
                                   void **out_result, size_t *out_len)
{
    const char *const *argv = def ? def->argv : NULL;
    int inp[2], outp[2];
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
    if (make_pipe_cloexec(inp) != 0) {
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }
    if (make_pipe_cloexec(outp) != 0) {
        close(inp[0]); close(inp[1]);
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }

    pid = fork();
    if (pid < 0) {
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
#if defined(__linux__)
        cgroup_destroy(cgdir);
#endif
        return GPTPS_E_IO;
    }

    if (pid == 0) {
        /* ---- CHILD: wire stdin/stdout to the pipes, cap memory, exec ---- */
        int joined = 0;
        dup2(inp[0], STDIN_FILENO);
        dup2(outp[1], STDOUT_FILENO);
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        setpgid(0, 0); /* own process group: a timeout kill takes down the program AND its children */
#if defined(__linux__)
        if (cgdir && cgroup_self_join(cgdir) == 0) joined = 1; /* accurate RSS cap before exec */
#endif
        if (!joined) apply_as_cap(mem_cap);                    /* coarse fallback */
        if (def->child_setup) def->child_setup(def->user_data); /* host hardening hook (chdir/seccomp/drop-privs/...) */
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
        int killed = 0, oversize = 0, nomem = 0, wstatus = 0;
        char *buf = NULL; size_t cap = 0, len = 0;
        const char *wp = (const char *)payload;   /* unwritten payload cursor */
        size_t wleft = plen;
        int in_open = 1;                           /* inp[1] still open for writing */
        uint64_t deadline = timeout_s ? gptps_hal_monotonic_ms() + (uint64_t)timeout_s * 1000u : 0;
#if !defined(F_SETNOSIGPIPE)
        sigset_t sp_old; int sp_masked = 0;
#endif

        close(inp[0]); close(outp[1]);
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
        if (!wleft) { close(inp[1]); in_open = 0; } /* no payload: EOF the child's stdin now */

        for (;;) {
            struct pollfd pfd[2]; int nfd = 0, oidx, iidx = -1, pr; int slice = 200;

            oidx = nfd; pfd[nfd].fd = outp[0]; pfd[nfd].events = POLLIN;  pfd[nfd].revents = 0; nfd++;
            if (in_open) { iidx = nfd; pfd[nfd].fd = inp[1]; pfd[nfd].events = POLLOUT; pfd[nfd].revents = 0; nfd++; }

            if (deadline) {
                uint64_t now = gptps_hal_monotonic_ms();
                if (now >= deadline) { killed = 1; kill(-pid, SIGKILL); break; }
                if (deadline - now < (uint64_t)slice) slice = (int)(deadline - now);
            }
            pr = poll(pfd, (nfds_t)nfd, slice);
            if (cancel && gptps_flag_get(cancel)) { killed = 1; kill(-pid, SIGKILL); break; }
            /* a genuine poll error (e.g. ENOMEM) must still kill the child, or the
             * blocking waitpid below would hang this worker on a still-live child -
             * the very thing this executor promises never to do. */
            if (pr < 0) { if (errno == EINTR) continue; kill(-pid, SIGKILL); break; }
            if (pr == 0) continue;                 /* slice elapsed: re-check deadline/cancel */

            /* drain stdout */
            if (pfd[oidx].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r;
                if (len == cap) {
                    size_t ncap = cap ? cap * 2 : 65536;
                    char *nb;
                    if (ncap > GPTPS_PROG_RESULT_CAP) ncap = GPTPS_PROG_RESULT_CAP;
                    if (ncap == cap) { oversize = 1; kill(-pid, SIGKILL); break; } /* >16 MiB */
                    nb = (char *)gptps_realloc(buf, ncap);
                    if (!nb) { nomem = 1; kill(-pid, SIGKILL); break; }
                    buf = nb; cap = ncap;
                }
                r = read(outp[0], buf + len, cap - len);
                if (r > 0) len += (size_t)r;
                else if (r == 0) break;            /* stdout EOF: child is done */
                else if (errno != EINTR && errno != EAGAIN) { kill(-pid, SIGKILL); break; } /* I/O error: kill so waitpid can't hang */
            }
            /* feed stdin */
            if (in_open && (pfd[iidx].revents & (POLLOUT | POLLERR | POLLHUP))) {
                if (pfd[iidx].revents & (POLLERR | POLLHUP)) {   /* child closed its stdin */
                    close(inp[1]); in_open = 0;
                } else {
                    ssize_t w = write(inp[1], wp, wleft);
                    if (w > 0) { wp += w; wleft -= (size_t)w; if (!wleft) { close(inp[1]); in_open = 0; } }
                    else if (w < 0 && errno != EINTR && errno != EAGAIN) { close(inp[1]); in_open = 0; }
                }
            }
        }
        if (in_open) close(inp[1]);
        close(outp[0]);
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
        while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) { }

        if (killed)                  eff = GPTPS_E_TIMEOUT;
        else if (oversize)           eff = GPTPS_E_IO;
        else if (nomem)              eff = GPTPS_E_NOMEM;
        else if (WIFEXITED(wstatus)) eff = (WEXITSTATUS(wstatus) == 0) ? GPTPS_OK : GPTPS_E_TASK;
        else                         eff = GPTPS_E_TASK; /* killed by a signal */
#if defined(__linux__)
        if (cgdir && eff != GPTPS_OK && eff != GPTPS_E_TIMEOUT && cgroup_oom_killed(cgdir))
            eff = GPTPS_E_NOMEM;     /* exceeded the memory cap */
        cgroup_destroy(cgdir);
#endif

        if (eff == GPTPS_OK) { *out_result = buf; *out_len = len; }
        else gptps_free(buf);
        return eff;
    }
}

#endif /* !_WIN32 */
