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
 *
 * Measurements (docs/MEASUREMENTS.md): the job object accounts for every process in
 * it, so the program and everything it starts are measured together - memory as
 * COMMITTED (what the processes reserved), not resident. Without a job object, the
 * program's own process handle is measured instead, and the method says so. Hitting
 * the memory cap is not reported: Windows announces it on a completion port whose
 * delivery it does not guarantee, and a missed message would read as "not hit".
 */
#if defined(_WIN32)

#include "gptps.h"
#include "gptps_internal.h"

#include <windows.h>
#include <psapi.h>     /* PROCESS_MEMORY_COUNTERS_EX: the type only; see pmi_fn */
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
                               uint64_t mem_cap, uint32_t timeout_s, const uint32_t *cancel,
                               void **out_result, size_t *out_len, gptps_exec_meter *meter)
{
    (void)def; (void)payload; (void)plen; (void)mem_cap; (void)timeout_s; (void)cancel; (void)meter;
    *out_result = NULL; *out_len = 0;
    return GPTPS_E_INVAL; /* fork-based isolation is POSIX-only */
}

/* ---- measurements (docs/MEASUREMENTS.md) ----------------------------------
 * Names and methods are string literals, so the meter's entries outlive this file's
 * frames; gptps_meter_put keeps the first value per name, so the job object's view
 * goes in before the process handle's. */

/* K32GetProcessMemoryInfo lives in kernel32 from Windows 7 on. Looked up at run time
 * so the library neither links psapi nor stops loading on an older system, where the
 * memory figures that need it are simply not reported. */
typedef BOOL (WINAPI *pmi_fn)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
static pmi_fn get_pmi(void)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    return k ? (pmi_fn)(void (*)(void))GetProcAddress(k, "K32GetProcessMemoryInfo") : NULL;
}

static uint64_t ft_ms(LARGE_INTEGER t100ns) { return (uint64_t)t100ns.QuadPart / 10000u; }
static uint64_t filetime_ms(FILETIME f)
{
    ULARGE_INTEGER u;
    u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime;
    return u.QuadPart / 10000u;   /* 100 ns units */
}

/* What the attempt used, after its processes have ended. The job object covers the
 * program and everything it started; the process handle covers the program alone.
 * Both are read, and two facts that need no guessing decide which is reported:
 *   - a job contains its program, so a job figure of 0 where the program's own is
 *     above 0 is not a measurement (an emulation such as wine answers the job
 *     queries with zeros);
 *   - every process commits memory, so a committed peak of 0 is not one either.
 * Only that contradiction counts. Job and process accounting are kept separately and
 * may differ by a tick as the program exits; switching method over that would make
 * one task's values flip between methods from attempt to attempt. Where neither
 * figure holds up, the name is left out (docs/MEASUREMENTS.md, rule 2). */
static void win_measure(gptps_exec_meter *mt, HANDLE job, int assigned, HANDLE proc)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION x;
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION a;
    PROCESS_MEMORY_COUNTERS_EX pm;
    FILETIME c, ex, k, u;
    IO_COUNTERS io;
    pmi_fn pmi = get_pmi();
    int have_x = 0, have_a = 0, have_pm = 0, have_t = 0, have_io = 0;
    int p_ran = 0;                     /* the program used any CPU time, unrounded */
    uint64_t p_peak = 0, p_user = 0, p_sys = 0, p_rd = 0, p_wr = 0;
    if (!mt) return;
    if (proc) {
        memset(&pm, 0, sizeof pm);
        pm.cb = sizeof pm;
        if (pmi && pmi(proc, (PPROCESS_MEMORY_COUNTERS)&pm, sizeof pm)) { have_pm = 1; p_peak = (uint64_t)pm.PeakPagefileUsage; }
        if (GetProcessTimes(proc, &c, &ex, &k, &u)) {
            have_t = 1; p_user = filetime_ms(u); p_sys = filetime_ms(k);
            p_ran = (u.dwLowDateTime | u.dwHighDateTime | k.dwLowDateTime | k.dwHighDateTime) != 0;
        }
        if (GetProcessIoCounters(proc, &io)) {
            have_io = 1; p_rd = (uint64_t)io.ReadTransferCount; p_wr = (uint64_t)io.WriteTransferCount;
        }
    }
    if (job && assigned) {
        memset(&x, 0, sizeof x);
        memset(&a, 0, sizeof a);
        have_x = QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &x, sizeof x, NULL) != 0;
        have_a = QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation, &a, sizeof a, NULL) != 0;
    }
    /* mem.peak */
    if (have_x && x.PeakJobMemoryUsed > 0)
        gptps_meter_put(mt, GPTPS_M_MEM_PEAK, (uint64_t)x.PeakJobMemoryUsed,
                        GPTPS_UNIT_BYTES, GPTPS_MEASURE_PEAK, "jobobject.tree.committed");
    else if (have_pm && p_peak > 0)
        gptps_meter_put(mt, GPTPS_M_MEM_PEAK, p_peak, GPTPS_UNIT_BYTES, GPTPS_MEASURE_PEAK, "process.committed");
    /* cpu.* */
    if (have_a && !(a.BasicInfo.TotalUserTime.QuadPart == 0 && a.BasicInfo.TotalKernelTime.QuadPart == 0 &&
                    have_t && p_ran)) {
        gptps_meter_put(mt, GPTPS_M_CPU_USER_MS, ft_ms(a.BasicInfo.TotalUserTime),
                        GPTPS_UNIT_MS, GPTPS_MEASURE_TOTAL, "jobobject.tree");
        gptps_meter_put(mt, GPTPS_M_CPU_SYS_MS, ft_ms(a.BasicInfo.TotalKernelTime),
                        GPTPS_UNIT_MS, GPTPS_MEASURE_TOTAL, "jobobject.tree");
    } else if (have_t) {
        gptps_meter_put(mt, GPTPS_M_CPU_USER_MS, p_user, GPTPS_UNIT_MS, GPTPS_MEASURE_TOTAL, "process");
        gptps_meter_put(mt, GPTPS_M_CPU_SYS_MS,  p_sys,  GPTPS_UNIT_MS, GPTPS_MEASURE_TOTAL, "process");
    }
    /* io.* */
    if (have_a && !(a.IoInfo.ReadTransferCount == 0 && a.IoInfo.WriteTransferCount == 0 &&
                    have_io && (p_rd > 0 || p_wr > 0))) {
        gptps_meter_put(mt, GPTPS_M_IO_READ_BYTES, (uint64_t)a.IoInfo.ReadTransferCount,
                        GPTPS_UNIT_BYTES, GPTPS_MEASURE_TOTAL, "jobobject.tree.all");
        gptps_meter_put(mt, GPTPS_M_IO_WRITE_BYTES, (uint64_t)a.IoInfo.WriteTransferCount,
                        GPTPS_UNIT_BYTES, GPTPS_MEASURE_TOTAL, "jobobject.tree.all");
    } else if (have_io) {
        gptps_meter_put(mt, GPTPS_M_IO_READ_BYTES,  p_rd, GPTPS_UNIT_BYTES, GPTPS_MEASURE_TOTAL, "process.all");
        gptps_meter_put(mt, GPTPS_M_IO_WRITE_BYTES, p_wr, GPTPS_UNIT_BYTES, GPTPS_MEASURE_TOTAL, "process.all");
    }
}

/* The committed memory of every process in the job right now, or 0 with *ok = 0 when
 * it cannot be known whole: more processes than the list holds, or one that cannot be
 * read. A process that exits between the list and the read has no memory left. */
#define GPTPS_WIN_SAMPLE_PIDS 64
static uint64_t job_committed_now(HANDLE job, int *ok)
{
    struct { JOBOBJECT_BASIC_PROCESS_ID_LIST h; ULONG_PTR more[GPTPS_WIN_SAMPLE_PIDS - 1]; } pl;
    pmi_fn pmi = get_pmi();
    uint64_t sum = 0;
    DWORD i;
    *ok = 0;
    if (!pmi) return 0;
    memset(&pl, 0, sizeof pl);
    if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, &pl, sizeof pl, NULL)) return 0;
    if (pl.h.NumberOfProcessIdsInList < pl.h.NumberOfAssignedProcesses) return 0;
    for (i = 0; i < pl.h.NumberOfProcessIdsInList; ++i) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE,
                               (DWORD)pl.h.ProcessIdList[i]);
        PROCESS_MEMORY_COUNTERS_EX pm;
        if (!h) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue;   /* already gone */
            return 0;                                                    /* cannot read it */
        }
        memset(&pm, 0, sizeof pm);
        pm.cb = sizeof pm;
        if (!pmi(h, (PPROCESS_MEMORY_COUNTERS)&pm, sizeof pm)) { CloseHandle(h); return 0; }
        sum += (uint64_t)pm.PrivateUsage;
        CloseHandle(h);
    }
    *ok = sum > 0;                    /* a running job commits memory: 0 is not a reading */
    return sum;
}

/* Samples of a running job, about every sample_ms (at least 10), from the wait loop. */
typedef struct { gptps_exec_meter *mt; uint64_t next; uint32_t iv; } win_sampler;

static void win_sampler_init(win_sampler *sm, gptps_exec_meter *mt)
{
    sm->mt = (mt && mt->sample_ms && mt->sample) ? mt : NULL;
    sm->iv = sm->mt ? (mt->sample_ms < 10u ? 10u : mt->sample_ms) : 0;
    sm->next = sm->mt ? gptps_hal_monotonic_ms() + sm->iv : 0;
}

static DWORD win_sampler_slice(const win_sampler *sm, DWORD slice)
{
    uint64_t now;
    if (!sm->mt) return slice;
    now = gptps_hal_monotonic_ms();
    if (now >= sm->next) return 0;
    return (sm->next - now < (uint64_t)slice) ? (DWORD)(sm->next - now) : slice;
}

static void win_sampler_tick(win_sampler *sm, HANDLE job, int assigned, HANDLE proc)
{
    gptps_measure cur[1];
    uint64_t now;
    int ok = 0;
    if (!sm->mt) return;
    now = gptps_hal_monotonic_ms();
    if (now < sm->next) return;
    sm->next = now + sm->iv;
    if (job && assigned) {
        cur[0].value = job_committed_now(job, &ok);
        cur[0].method = "jobobject.tree.committed";
    }
    if (!ok) {                        /* no job object, or no whole reading of it */
        pmi_fn pmi = get_pmi();
        PROCESS_MEMORY_COUNTERS_EX pm;
        memset(&pm, 0, sizeof pm);
        pm.cb = sizeof pm;
        if (pmi && pmi(proc, (PPROCESS_MEMORY_COUNTERS)&pm, sizeof pm) && pm.PrivateUsage > 0) {
            cur[0].value = (uint64_t)pm.PrivateUsage;
            cur[0].method = "process.committed";
            ok = 1;
        }
    }
    if (!ok) return;
    cur[0].name = GPTPS_M_MEM_CURRENT;
    cur[0].unit = GPTPS_UNIT_BYTES; cur[0].kind = GPTPS_MEASURE_CURRENT; cur[0].flags = 0;
    sm->mt->sample(sm->mt, cur, 1);
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

typedef struct { HANDLE h; const char *data; size_t len; int ioerr;
                 HANDLE proc, job; int assigned; } writer_ctx;
static DWORD WINAPI writer_proc(LPVOID p)
{
    writer_ctx *w = (writer_ctx *)p;
    size_t off = 0; DWORD wr;
    while (off < w->len) {
        if (!WriteFile(w->h, w->data + off, (DWORD)(w->len - off), &wr, NULL)) {
            DWORD e = GetLastError();
            /* ERROR_NO_DATA / ERROR_BROKEN_PIPE: the child closed its stdin, so it wants
             * no more of it (EPIPE in exec_oop_posix.c). ERROR_OPERATION_ABORTED is the
             * parent's own CancelSynchronousIo, after the child has exited. Any other
             * error means the payload cannot be delivered: closing stdin as if it were
             * all of it would hand the program a truncated payload, and one that exits
             * 0 on that is a success with the wrong result. Stop the child before it
             * sees EOF (termination is asynchronous, hence the wait), and the attempt
             * fails with GPTPS_E_IO, as on POSIX. */
            if (e != ERROR_NO_DATA && e != ERROR_BROKEN_PIPE && e != ERROR_OPERATION_ABORTED) {
                w->ioerr = 1;
                if (w->assigned) TerminateJobObject(w->job, 1);
                else             TerminateProcess(w->proc, 1);
                WaitForSingleObject(w->proc, GPTPS_WIN_JOIN_GRACE_MS);
            }
            break;
        }
        if (wr == 0) break;
        off += wr;
    }
    CloseHandle(w->h); /* EOF on the child's stdin */
    return 0;
}

typedef struct { HANDLE h; char *buf; size_t len, cap; int nomem, oversize, ioerr;
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
        BOOL ok;
        char probe;                       /* where a byte past the cap lands */
        int full = 0;                     /* the buffer is at the cap */
        if (r->len == r->cap) {
            size_t nc = r->cap ? r->cap * 2 : 65536;
            char *nb;
            if (nc > GPTPS_WIN_RESULT_CAP) nc = GPTPS_WIN_RESULT_CAP;
            full = (nc == r->cap);
            if (!full) {
                nb = (char *)gptps_realloc(r->buf, nc);
                if (!nb) { r->nomem = 1; reader_stop_child(r); break; }
                r->buf = nb; r->cap = nc;
            }
        }
        /* At the cap, read one byte into `probe`: the pipe closing there is a result of
         * exactly the cap, which is allowed, and only a byte past it is oversize - as
         * in exec_oop_posix.c. Refusing as soon as the buffer filled turned a result of
         * exactly 16 MiB into GPTPS_E_IO. */
        ok = full ? ReadFile(r->h, &probe, 1, &got, NULL)
                  : ReadFile(r->h, r->buf + r->len, (DWORD)(r->cap - r->len), &got, NULL);
        if (!ok) {
            DWORD e = GetLastError();
            /* ERROR_BROKEN_PIPE / ERROR_HANDLE_EOF: every write end is closed, the end
             * of the output. ERROR_OPERATION_ABORTED and ERROR_INVALID_HANDLE are the
             * parent's own teardown, after the child has exited: CancelSynchronousIo,
             * then closing outR. Any other error means the output cannot be read
             * whole: taking it for the end would make a success of part of it. Stop
             * the child, and the attempt fails with GPTPS_E_IO, as a stdout read error
             * does in exec_oop_posix.c. */
            if (e != ERROR_BROKEN_PIPE && e != ERROR_HANDLE_EOF &&
                e != ERROR_OPERATION_ABORTED && e != ERROR_INVALID_HANDLE) {
                r->ioerr = 1;
                reader_stop_child(r);
            }
            break;
        }
        if (got == 0) break;              /* end of the output */
        if (full) { r->oversize = 1; reader_stop_child(r); break; }   /* >16 MiB */
        r->len += got;
    }
    return 0;
}

gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s, const uint32_t *cancel,
                                   void **out_result, size_t *out_len, gptps_exec_meter *meter)
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

    wc.h = inW; wc.data = (const char *)payload; wc.len = plen; wc.ioerr = 0;
    wc.proc = pi.hProcess; wc.job = job; wc.assigned = assigned;
    rc.h = outR; rc.buf = NULL; rc.len = rc.cap = 0; rc.nomem = rc.oversize = rc.ioerr = 0;
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
        win_sampler sm;
        win_sampler_init(&sm, meter);
        for (;;) {
            DWORD slice = 200;
            win_sampler_tick(&sm, job, assigned, pi.hProcess);   /* a sample, when due */
            if (deadline) {
                uint64_t now = gptps_hal_monotonic_ms();
                if (now >= deadline) { killed = 1; kill_st = GPTPS_E_TIMEOUT; break; }
                if (deadline - now < (uint64_t)slice) slice = (DWORD)(deadline - now);
            }
            slice = win_sampler_slice(&sm, slice);
            waited = WaitForSingleObject(pi.hProcess, slice);
            if (waited == WAIT_OBJECT_0) break;                       /* child exited */
            /* An explicit gptps_cancel / shutdown / task removal is NOT a deadline
             * breach - report the two apart so an operator can tell which happened. */
            if (cancel && gptps_hal_load_acquire_u32(cancel)) { killed = 1; kill_st = GPTPS_E_CANCELLED; break; }
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
    /* Every process of the job has ended (TerminateJobObject above), so its totals are
     * final; the handles are still open, which is all the queries need. */
    win_measure(meter, job, assigned, pi.hProcess);

    if (outR) CloseHandle(outR);          /* no reader thread was ever started */
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);

    if      (killed)        eff = kill_st;
    else if (rc.oversize)   eff = GPTPS_E_IO;
    else if (wc.ioerr)      eff = GPTPS_E_IO;   /* the payload could not be delivered */
    else if (rc.ioerr)      eff = GPTPS_E_IO;   /* the output could not be read */
    else if (rc.nomem)      eff = GPTPS_E_NOMEM;
    else                    eff = (code == 0) ? GPTPS_OK : GPTPS_E_TASK;

    if (eff == GPTPS_OK) { *out_result = rc.buf; *out_len = rc.len; }
    else gptps_free(rc.buf);
    return eff;
}

#endif /* _WIN32 */
