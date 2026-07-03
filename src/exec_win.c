/*
 * exec_win.c - Windows executor backend (counterpart to exec_oop_posix.c).
 *
 *  - gptps_program_execute: the Win32 mapping of "run an external program as a
 *    task" - CreateProcess with the payload piped to stdin and stdout captured as
 *    the result, wrapped in a Job Object (optional memory cap + kill-on-close) and
 *    hard-killed on the deadline. The genuinely-enforced, language-agnostic path.
 *  - gptps_oop_execute: the OOP executor runs an in-process task FUNCTION inside an
 *    isolated child, which is built on POSIX fork(); Windows has no equivalent, so
 *    it reports GPTPS_E_INVAL (EXEC_OOP is POSIX-only).
 */
#if defined(_WIN32)

#include "gptps.h"
#include "gptps_internal.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define GPTPS_WIN_MEMCAP_FLOOR (16ull * 1024ull * 1024ull) /* below this, a mem cap is meaningless */
#define GPTPS_WIN_RESULT_CAP   (16u * 1024u * 1024u)        /* max captured stdout */

gptps_status gptps_oop_execute(const gptps_task_def *def, const void *payload, size_t plen,
                               uint64_t mem_cap, uint32_t timeout_s, gptps_flag *cancel,
                               void **out_result, size_t *out_len)
{
    (void)def; (void)payload; (void)plen; (void)mem_cap; (void)timeout_s; (void)cancel;
    *out_result = NULL; *out_len = 0;
    return GPTPS_E_INVAL; /* fork-based isolation is POSIX-only */
}

/* Quote argv into one CreateProcess command line (MSDN argv parsing rules). */
static char *build_cmdline(const char *const *argv)
{
    size_t cap = 1, i;
    char *out, *w;
    for (i = 0; argv[i]; ++i) cap += 2 * strlen(argv[i]) + 3;
    out = (char *)gptps_malloc(cap);
    if (!out) return NULL;
    w = out;
    for (i = 0; argv[i]; ++i) {
        const char *a = argv[i];
        int quote = (*a == 0) || strpbrk(a, " \t\"") != NULL;
        if (i) *w++ = ' ';
        if (quote) *w++ = '"';
        while (*a) {
            size_t bs = 0, k;
            while (*a == '\\') { ++bs; ++a; }
            if (*a == 0) { for (k = 0; k < bs * 2; ++k) *w++ = '\\'; break; }      /* before closing quote */
            else if (*a == '"') { for (k = 0; k < bs * 2 + 1; ++k) *w++ = '\\'; *w++ = '"'; ++a; }
            else { for (k = 0; k < bs; ++k) *w++ = '\\'; *w++ = *a++; }
        }
        if (quote) *w++ = '"';
    }
    *w = 0;
    return out;
}

typedef struct { HANDLE h; const char *data; size_t len; } writer_ctx;
static DWORD WINAPI writer_proc(LPVOID p)
{
    writer_ctx *w = (writer_ctx *)p;
    size_t off = 0; DWORD wr;
    while (off < w->len) {
        if (!WriteFile(w->h, w->data + off, (DWORD)(w->len - off), &wr, NULL) || wr == 0) break;
        off += wr;
    }
    CloseHandle(w->h); /* EOF on the child's stdin */
    return 0;
}

typedef struct { HANDLE h; char *buf; size_t len, cap; int nomem, oversize; } reader_ctx;
static DWORD WINAPI reader_proc(LPVOID p)
{
    reader_ctx *r = (reader_ctx *)p;
    for (;;) {
        DWORD got;
        if (r->len == r->cap) {
            size_t nc = r->cap ? r->cap * 2 : 65536;
            char *nb;
            if (nc > GPTPS_WIN_RESULT_CAP) nc = GPTPS_WIN_RESULT_CAP;
            if (nc == r->cap) { r->oversize = 1; break; }
            nb = (char *)gptps_realloc(r->buf, nc);
            if (!nb) { r->nomem = 1; break; }
            r->buf = nb; r->cap = nc;
        }
        if (!ReadFile(r->h, r->buf + r->len, (DWORD)(r->cap - r->len), &got, NULL)) break; /* pipe closed */
        if (got == 0) break;
        r->len += got;
    }
    return 0;
}

gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s, gptps_flag *cancel,
                                   void **out_result, size_t *out_len)
{
    const char *const *argv = def ? def->argv : NULL;
    SECURITY_ATTRIBUTES sa;
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL, job = NULL, wt = NULL, rt = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    writer_ctx wc;
    reader_ctx rc;
    char *cmd;
    DWORD code = 1, waited;
    int killed = 0, assigned = 0;
    gptps_status eff;

    /* def->child_setup is a POSIX fork-time hook; Windows has no fork model, so
     * it does not apply to CreateProcess and is intentionally ignored here. */
    *out_result = NULL; *out_len = 0;
    if (!argv || !argv[0]) return GPTPS_E_INVAL;

    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&inR, &inW, &sa, 0)) return GPTPS_E_IO;
    if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW); return GPTPS_E_IO; }
    /* parent-side ends must NOT be inherited by the child */
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof jeli);
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (mem_cap >= GPTPS_WIN_MEMCAP_FLOOR) {
            jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            jeli.ProcessMemoryLimit = (SIZE_T)mem_cap;
        }
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof jeli);
    }

    cmd = build_cmdline(argv);
    if (!cmd) { CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
                if (job) CloseHandle(job);
return GPTPS_E_NOMEM; }

    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = inR;
    si.hStdOutput = outW;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        gptps_free(cmd);
        CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
        if (job) CloseHandle(job);
        return GPTPS_E_TASK; /* program could not be started */
    }
    gptps_free(cmd);
    CloseHandle(inR); CloseHandle(outW); /* child owns these; parent keeps inW + outR */

    if (job && AssignProcessToJobObject(job, pi.hProcess)) assigned = 1;
    ResumeThread(pi.hThread);

    wc.h = inW; wc.data = (const char *)payload; wc.len = plen;
    rc.h = outR; rc.buf = NULL; rc.len = rc.cap = 0; rc.nomem = rc.oversize = 0;
    wt = CreateThread(NULL, 0, writer_proc, &wc, 0, NULL);
    if (!wt) CloseHandle(inW);                 /* no writer => close stdin so the child sees EOF */
    rt = CreateThread(NULL, 0, reader_proc, &rc, 0, NULL);

    /* Wait for the child in bounded slices so a raised cancel flag (gptps_cancel /
     * shutdown / task removal) OR the deadline hard-kills it - including when
     * timeout_s==0 (no deadline), which otherwise waited forever and could not be
     * cancelled. The writer/reader threads run concurrently, so there is no stdin/
     * stdout deadlock to solve here (unlike the POSIX single-thread pump). */
    {
        uint64_t deadline = timeout_s ? gptps_hal_monotonic_ms() + (uint64_t)timeout_s * 1000u : 0;
        for (;;) {
            DWORD slice = 200;
            if (deadline) {
                uint64_t now = gptps_hal_monotonic_ms();
                if (now >= deadline) { killed = 1; break; }
                if (deadline - now < (uint64_t)slice) slice = (DWORD)(deadline - now);
            }
            waited = WaitForSingleObject(pi.hProcess, slice);
            if (waited == WAIT_OBJECT_0) break;                       /* child exited */
            if (cancel && gptps_flag_get(cancel)) { killed = 1; break; } /* cancelled */
            if (waited == WAIT_FAILED) { killed = 1; break; }        /* defensive: never spin */
            /* WAIT_TIMEOUT: slice elapsed, loop and re-check deadline/cancel */
        }
        if (killed) {
            if (assigned) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
        }
    }
    WaitForSingleObject(pi.hProcess, INFINITE); /* fully gone => its stdout closes => reader ends */
    if (wt) { WaitForSingleObject(wt, INFINITE); CloseHandle(wt); }
    if (rt) { WaitForSingleObject(rt, INFINITE); CloseHandle(rt); }
    GetExitCodeProcess(pi.hProcess, &code);

    CloseHandle(outR);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);

    if      (killed)        eff = GPTPS_E_TIMEOUT;
    else if (rc.oversize)   eff = GPTPS_E_IO;
    else if (rc.nomem)      eff = GPTPS_E_NOMEM;
    else                    eff = (code == 0) ? GPTPS_OK : GPTPS_E_TASK;

    if (eff == GPTPS_OK) { *out_result = rc.buf; *out_len = rc.len; }
    else gptps_free(rc.buf);
    return eff;
}

#endif /* _WIN32 */
