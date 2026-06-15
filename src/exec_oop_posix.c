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
#include "gptps.h"
#include "gptps_internal.h"

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define GPTPS_OOP_MEMCAP_FLOOR (16ull * 1024ull * 1024ull) /* below this, AS cap is meaningless */

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
                               uint64_t mem_cap, uint32_t timeout_s,
                               void **out_result, size_t *out_len)
{
    int p[2];
    pid_t pid;

    *out_result = NULL; *out_len = 0;
    if (pipe(p) != 0) return GPTPS_E_IO;

    pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return GPTPS_E_IO; }

    if (pid == 0) {
        /* ---- CHILD ---- */
        void *res = NULL; size_t rlen = 0;
        int32_t st32; uint64_t len64;
        gptps_status st;
        signal(SIGPIPE, SIG_IGN);
        close(p[0]);
#if defined(RLIMIT_AS)
        if (mem_cap >= GPTPS_OOP_MEMCAP_FLOOR) {
            struct rlimit rl; rl.rlim_cur = (rlim_t)mem_cap; rl.rlim_max = (rlim_t)mem_cap;
            setrlimit(RLIMIT_AS, &rl); /* best-effort coarse cap (RLIMIT_AS absent on macOS) */
        }
#else
        (void)mem_cap;
#endif
        st = gptps_run_capture(def, payload, plen, &res, &rlen);
        st32 = (int32_t)st; len64 = (uint64_t)rlen;
        write_all(p[1], &st32, sizeof st32);
        write_all(p[1], &len64, sizeof len64);
        if (rlen) write_all(p[1], res, rlen);
        free(res);
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
        int tmo;

        close(p[1]);
        tmo = (timeout_s == 0) ? -1
            : (timeout_s > 2000000u ? -1 : (int)(timeout_s * 1000u));

        pfd.fd = p[0]; pfd.events = POLLIN; pfd.revents = 0;
        do { pr = poll(&pfd, 1, tmo); } while (pr < 0 && errno == EINTR);
        if (pr == 0) { kill(pid, SIGKILL); killed = 1; }      /* deadline -> hard kill */

        if (!killed) {
            if (read_all(p[0], &st32, sizeof st32) == 0 &&
                read_all(p[0], &len64, sizeof len64) == 0) {
                if (len64) {
                    res = malloc((size_t)len64);
                    if (!res || read_all(p[0], res, (size_t)len64) != 0) {
                        free(res); res = NULL; len64 = 0; st32 = (int32_t)GPTPS_E_IO;
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
            free(res); res = NULL; len64 = 0;   /* crash / OOM-kill */
            eff = GPTPS_E_TASK;
        } else {
            eff = (gptps_status)st32;
        }
        *out_result = res; *out_len = (size_t)len64;
        return eff;
    }
}

#define GPTPS_PROG_RESULT_CAP (16u * 1024u * 1024u) /* max captured stdout */

gptps_status gptps_program_execute(const char *const *argv, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s,
                                   void **out_result, size_t *out_len)
{
    int inp[2], outp[2];
    pid_t pid;

    *out_result = NULL; *out_len = 0;
    if (!argv || !argv[0]) return GPTPS_E_INVAL;
    if (pipe(inp) != 0) return GPTPS_E_IO;
    if (pipe(outp) != 0) { close(inp[0]); close(inp[1]); return GPTPS_E_IO; }

    pid = fork();
    if (pid < 0) { close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]); return GPTPS_E_IO; }

    if (pid == 0) {
        /* ---- CHILD: wire stdin/stdout to the pipes, cap memory, exec ---- */
        dup2(inp[0], STDIN_FILENO);
        dup2(outp[1], STDOUT_FILENO);
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        setpgid(0, 0); /* own process group: a timeout kill takes down the program AND its children */
#if defined(RLIMIT_AS)
        if (mem_cap >= GPTPS_OOP_MEMCAP_FLOOR) {
            struct rlimit rl; rl.rlim_cur = (rlim_t)mem_cap; rl.rlim_max = (rlim_t)mem_cap;
            setrlimit(RLIMIT_AS, &rl);
        }
#else
        (void)mem_cap;
#endif
        execv(argv[0], (char *const *)argv);
        _exit(127); /* exec failed */
    }

    /* ---- PARENT: feed payload to stdin, read stdout up to the deadline ---- */
    {
        gptps_status eff = GPTPS_OK;
        int killed = 0, oversize = 0, nomem = 0, wstatus = 0;
        char *buf = NULL; size_t cap = 0, len = 0;
        int tmo = (timeout_s == 0) ? -1 : (timeout_s > 2000000u ? -1 : (int)(timeout_s * 1000u));

        close(inp[0]); close(outp[1]);
        setpgid(pid, pid);                        /* idempotent with the child: race-free group setup */
        signal(SIGPIPE, SIG_IGN);                 /* program may exit before reading all stdin */
        if (plen) (void)write_all(inp[1], payload, plen);
        close(inp[1]);                            /* EOF on the child's stdin */

        for (;;) {
            struct pollfd pfd; int pr; ssize_t r;
            pfd.fd = outp[0]; pfd.events = POLLIN; pfd.revents = 0;
            pr = poll(&pfd, 1, tmo);
            if (pr < 0) { if (errno == EINTR) continue; break; }
            if (pr == 0) { killed = 1; kill(-pid, SIGKILL); break; }   /* idle past deadline: kill the group */
            if (len == cap) {
                size_t ncap = cap ? cap * 2 : 65536;
                char *nb;
                if (ncap > GPTPS_PROG_RESULT_CAP) ncap = GPTPS_PROG_RESULT_CAP;
                if (ncap == cap) { oversize = 1; kill(pid, SIGKILL); break; } /* >16 MiB */
                nb = (char *)realloc(buf, ncap);
                if (!nb) { nomem = 1; kill(pid, SIGKILL); break; }
                buf = nb; cap = ncap;
            }
            r = read(outp[0], buf + len, cap - len);
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) break;                    /* EOF: child closed stdout */
            len += (size_t)r;
        }
        close(outp[0]);
        while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) { }

        if (killed)                  eff = GPTPS_E_TIMEOUT;
        else if (oversize)           eff = GPTPS_E_IO;
        else if (nomem)              eff = GPTPS_E_NOMEM;
        else if (WIFEXITED(wstatus)) eff = (WEXITSTATUS(wstatus) == 0) ? GPTPS_OK : GPTPS_E_TASK;
        else                         eff = GPTPS_E_TASK; /* killed by a signal */

        if (eff == GPTPS_OK) { *out_result = buf; *out_len = len; }
        else free(buf);
        return eff;
    }
}
