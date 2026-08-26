/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
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
#define GPTPS_WIN_JOIN_GRACE_MS 5000  /* how long a helper thread gets to notice EOF */

/* CancelSynchronousIo is the only lever against a SYNCHRONOUS ReadFile/WriteFile
 * issued by another thread (CancelIoEx cancels ASYNC I/O and does not apply). It is
 * Vista+; on an older target there is simply no lever, so compile it out. */
#if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0600)
#  define GPTPS_WIN_CANCEL_SYNC_IO(h) ((void)CancelSynchronousIo(h))
#else
#  define GPTPS_WIN_CANCEL_SYNC_IO(h) ((void)(h))
#endif

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

typedef struct { HANDLE h; char *buf; size_t len, cap; int nomem, oversize;
                 HANDLE proc, job; int assigned; } reader_ctx;

/* When the reader stops early it must also stop the child, because nothing else
 * will: the parent never closes outR while the reader is alive, so a program that
 * keeps writing past the 16 MiB cap blocks in WriteFile forever and the task can
 * only end at its deadline - reporting GPTPS_E_TIMEOUT (or E_CANCELLED with
 * timeout_s==0) and hiding the oversize/nomem cause behind it, since `killed` is
 * tested first. POSIX already kills at the cap (exec_oop_posix.c: `oversize = 1;
 * kill(-pid, SIGKILL);`); this makes the two backends agree on what the README
 * sells as a hard cap. Do NOT close r->h here: outR is the PARENT's handle to close
 * (it does so just before joining this thread), and closing it twice is a bug. */
static void reader_stop_child(reader_ctx *r)
{
    if (r->assigned) TerminateJobObject(r->job, 1);
    else             TerminateProcess(r->proc, 1);
}

static DWORD WINAPI reader_proc(LPVOID p)
{
    reader_ctx *r = (reader_ctx *)p;
    for (;;) {
        DWORD got;
        if (r->len == r->cap) {
            size_t nc = r->cap ? r->cap * 2 : 65536;
            char *nb;
            if (nc > GPTPS_WIN_RESULT_CAP) nc = GPTPS_WIN_RESULT_CAP;
            if (nc == r->cap) { r->oversize = 1; reader_stop_child(r); break; }
            nb = (char *)gptps_realloc(r->buf, nc);
            if (!nb) { r->nomem = 1; reader_stop_child(r); break; }
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
    gptps_status kill_st = GPTPS_E_TIMEOUT;   /* why we killed the child, if we did */

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
    rc.proc = pi.hProcess; rc.job = job; rc.assigned = assigned;
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
                if (now >= deadline) { killed = 1; kill_st = GPTPS_E_TIMEOUT; break; }
                if (deadline - now < (uint64_t)slice) slice = (DWORD)(deadline - now);
            }
            waited = WaitForSingleObject(pi.hProcess, slice);
            if (waited == WAIT_OBJECT_0) break;                       /* child exited */
            /* An explicit gptps_cancel / shutdown / task removal is NOT a deadline
             * breach - report the two apart so an operator can tell which happened. */
            if (cancel && gptps_flag_get(cancel)) { killed = 1; kill_st = GPTPS_E_CANCELLED; break; }
            if (waited == WAIT_FAILED) { killed = 1; kill_st = GPTPS_E_IO; break; } /* defensive: never spin */
            /* WAIT_TIMEOUT: slice elapsed, loop and re-check deadline/cancel */
        }
        /* Tear the job down on EVERY path, not just the kill path, and do it BEFORE
         * the joins. "child gone => its stdout closes => reader ends" is false: an
         * anonymous pipe reports EOF only when the LAST write handle closes, so a
         * grandchild that inherited outW (`cmd /c start worker.exe`, say) keeps
         * reader_proc blocked in ReadFile long after the direct child exited - and
         * the INFINITE joins below would then wedge this worker, and the
         * gptps_shutdown that joins it, forever. JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
         * already kills exactly this set at CloseHandle(job); we only move that kill
         * earlier, so nothing survives that used to. The direct child's exit code is
         * already latched on pi.hProcess, so GetExitCodeProcess stays correct (and
         * TerminateProcess on an exited process is a no-op). */
        if (assigned) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
    }
    WaitForSingleObject(pi.hProcess, INFINITE); /* terminated above => bounded */

    /* The WRITER first, because it is the one whose handle the parent does not own:
     * writer_proc closes w->h itself to give the child EOF on stdin, so the parent
     * closing it too would be a double close. Killing the job/child normally breaks
     * the pipe and its WriteFile fails immediately; the grace plus
     * CancelSynchronousIo covers the case where it does not (assigned == 0 AND a
     * grandchild still holds the child's stdin read end). See the note below for the
     * residual. */
    if (wt) {
        if (WaitForSingleObject(wt, GPTPS_WIN_JOIN_GRACE_MS) == WAIT_TIMEOUT) GPTPS_WIN_CANCEL_SYNC_IO(wt);
        WaitForSingleObject(wt, INFINITE); CloseHandle(wt);
    }

    /* The READER's handle IS ours, so close it BEFORE joining rather than after.
     * That is the deterministic unblock: reader_proc's next ReadFile fails and the
     * thread returns. The alternative - joining first and closing after - is what
     * made this join unbounded, because an anonymous pipe signals EOF only when the
     * LAST write handle closes, so a grandchild that inherited outW kept the reader
     * parked in ReadFile with nothing left to end it. CancelSynchronousIo is kept as
     * a belt-and-braces first attempt (it is the documented mechanism), but it is
     * unreliable on anonymous pipes, which is exactly why the close is what the
     * bound actually rests on. rc.buf is untouched by the close and stays valid: it
     * lives in this frame and we still join before reading it. */
    if (rt) {
        if (WaitForSingleObject(rt, GPTPS_WIN_JOIN_GRACE_MS) == WAIT_TIMEOUT) GPTPS_WIN_CANCEL_SYNC_IO(rt);
        CloseHandle(outR);
        outR = NULL;
        WaitForSingleObject(rt, INFINITE); CloseHandle(rt);
    }
    GetExitCodeProcess(pi.hProcess, &code);

    if (outR) CloseHandle(outR);          /* no reader thread was ever started */
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);

    if      (killed)        eff = kill_st;
    else if (rc.oversize)   eff = GPTPS_E_IO;
    else if (rc.nomem)      eff = GPTPS_E_NOMEM;
    else                    eff = (code == 0) ? GPTPS_OK : GPTPS_E_TASK;

    if (eff == GPTPS_OK) { *out_result = rc.buf; *out_len = rc.len; }
    else gptps_free(rc.buf);
    return eff;
}

#endif /* _WIN32 */
