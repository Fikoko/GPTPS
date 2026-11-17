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
#include <stdio.h>     /* snprintf: the start-failure text */
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
                               gptps_exec_caps *caps, uint32_t timeout_s, const uint32_t *cancel,
                               void **out_result, size_t *out_len, gptps_exec_meter *meter)
{
    (void)def; (void)payload; (void)plen; (void)caps; (void)timeout_s; (void)cancel; (void)meter;
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

/* UTF-8 to UTF-16 for CreateProcessW (ABI 2.7). A string that is not valid UTF-8 is
 * read in the ANSI code page instead - what CreateProcessA made of every string - so a
 * host that passed ANSI text before 2.7 is started as it was. NULL when out of memory
 * or unconvertible. */
static wchar_t *win_wide(const char *s)
{
    UINT cp = CP_UTF8;
    DWORD fl = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, fl, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) { cp = CP_ACP; fl = 0; n = MultiByteToWideChar(cp, fl, s, -1, NULL, 0); }
    if (n <= 0) return NULL;
    w = (wchar_t *)gptps_malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    if (MultiByteToWideChar(cp, fl, s, -1, w, n) <= 0) { gptps_free(w); return NULL; }
    return w;
}

/* Quote wide argv into one CreateProcessW command line (MSDN argv parsing rules, which
 * CommandLineToArgvW and the C runtimes follow): backslashes are literal unless a quote
 * follows them. So a run of them is doubled only before a quote - an escaped one in the
 * argument, or the closing one of a quoted argument. An unquoted argument that ends in
 * '\' ("/out=D:\Reports\") keeps its backslashes as they are. */
static wchar_t *build_cmdline_w(wchar_t *const *wv, size_t n)
{
    size_t cap = 1, i;
    wchar_t *out, *w;
    for (i = 0; i < n; ++i) cap += 2 * wcslen(wv[i]) + 3;
    out = (wchar_t *)gptps_malloc(cap * sizeof *out);
    if (!out) return NULL;
    w = out;
    for (i = 0; i < n; ++i) {
        const wchar_t *a = wv[i];
        int quote = (*a == 0) || wcspbrk(a, L" \t\"") != NULL;
        if (i) *w++ = L' ';
        if (quote) *w++ = L'"';
        while (*a) {
            size_t bs = 0, k;
            while (*a == L'\\') { ++bs; ++a; }
            if (*a == 0) { for (k = 0; k < (quote ? bs * 2 : bs); ++k) *w++ = L'\\'; break; } /* before closing quote */
            else if (*a == L'"') { for (k = 0; k < bs * 2 + 1; ++k) *w++ = L'\\'; *w++ = L'"'; ++a; }
            else { for (k = 0; k < bs; ++k) *w++ = L'\\'; *w++ = *a++; }
        }
        if (quote) *w++ = L'"';
    }
    *w = 0;
    return out;
}

/* Names compare as Windows compares them: ordinally, ignoring case - CompareStringOrdinal
 * where the system has it (Vista and later), else the C library's case-folding. */
typedef int (WINAPI *cso_fn)(LPCWCH, int, LPCWCH, int, BOOL);
static int wname_cmp(const wchar_t *a, size_t la, const wchar_t *b, size_t lb)
{
    static cso_fn cso;
    static int looked;
    if (!looked) { cso = (cso_fn)(void (*)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"), "CompareStringOrdinal"); looked = 1; }
    if (cso) {
        int r = cso(a, (int)la, b, (int)lb, TRUE);
        if (r) return r - 2;                       /* CSTR_LESS_THAN 1, EQUAL 2, GREATER 3 */
    }
    {
        size_t n = la < lb ? la : lb;
        int r = _wcsnicmp(a, b, n);
        return r ? r : (la < lb ? -1 : la > lb ? 1 : 0);
    }
}

/* the name's length in "NAME=value" or a bare "NAME"; Windows' own per-drive entries
 * ("=C:=C:\\x") begin with '=', which is part of their name */
static size_t wenv_name_len(const wchar_t *s)
{
    const wchar_t *q = s[0] ? wcschr(s + 1, L'=') : NULL;
    return q ? (size_t)(q - s) : wcslen(s);
}
static int wenv_same(const wchar_t *a, size_t la, const wchar_t *b, size_t lb)
{
    return la == lb && wname_cmp(a, la, b, lb) == 0;   /* Windows names ignore case */
}
static int wenv_order(const void *x, const void *y)
{
    const wchar_t *a = *(const wchar_t *const *)x, *b = *(const wchar_t *const *)y;
    return wname_cmp(a, wenv_name_len(a), b, wenv_name_len(b));
}

/* The program's environment block (ABI 2.7 gptps_task_def.env): the host's, with each
 * "NAME=value" replacing or adding NAME and each bare "NAME" removing it - SORTED by
 * name, as Windows documents a block must be (programs that bisect it find a variable
 * only then). *path_out (may be NULL) gets the PATH the program will see, as a copy,
 * when `over` sets one; NULL otherwise. NULL when out of memory. */
static wchar_t *win_env_block(const char *const *over, wchar_t **path_out)
{
    wchar_t *host, *p, *blk = NULL, *w, **ov, **all = NULL;
    size_t no = 0, nh = 0, na = 0, i, j, total = 2;
    if (path_out) *path_out = NULL;
    while (over[no]) ++no;
    ov = (wchar_t **)gptps_calloc(no + 1, sizeof *ov);
    if (!ov) return NULL;
    for (j = 0; j < no; ++j) {
        if (!(ov[j] = win_wide(over[j]))) goto out;
        total += wcslen(ov[j]) + 1;
    }
    host = GetEnvironmentStringsW();
    for (p = host; p && *p; p += wcslen(p) + 1) { total += wcslen(p) + 1; ++nh; }
    all = (wchar_t **)gptps_calloc(nh + no + 1, sizeof *all);
    blk = all ? (wchar_t *)gptps_malloc(total * sizeof *blk) : NULL;
    if (blk) {
        for (p = host; p && *p; p += wcslen(p) + 1) {
            size_t L = wenv_name_len(p);
            int replaced = 0;
            for (j = 0; j < no && !replaced; ++j) replaced = wenv_same(ov[j], wenv_name_len(ov[j]), p, L);
            if (!replaced) all[na++] = p;
        }
        for (j = 0; j < no; ++j) {
            size_t L = wenv_name_len(ov[j]), k;
            int later = 0;          /* the last entry for a name wins */
            if (!wcschr(ov[j] + 1, L'=')) continue;            /* a removal */
            for (k = j + 1; k < no && !later; ++k) later = wenv_same(ov[k], wenv_name_len(ov[k]), ov[j], L);
            if (later) continue;
            all[na++] = ov[j];
            if (path_out && wenv_same(ov[j], L, L"PATH", 4)) {
                size_t vn = wcslen(ov[j] + L + 1) + 1;
                if ((*path_out = (wchar_t *)gptps_malloc(vn * sizeof **path_out)) != NULL)
                    memcpy(*path_out, ov[j] + L + 1, vn * sizeof **path_out);
            }
        }
        qsort(all, na, sizeof *all, wenv_order);
        w = blk;
        for (i = 0; i < na; ++i) { size_t n = wcslen(all[i]) + 1; memcpy(w, all[i], n * sizeof *w); w += n; }
        if (w == blk) *w++ = 0;   /* an empty block is still two NULs */
        *w = 0;
    }
    gptps_free(all);
    if (host) FreeEnvironmentStringsW(host);
out:
    for (i = 0; i < no; ++i) gptps_free(ov[i]);
    gptps_free(ov);
    return blk;
}

/* ABI 2.8: find the program the way POSIX does, on Windows too. A RELATIVE path with a
 * separator is relative to work_dir, when there is one (POSIX enters work_dir before
 * exec). A bare name is looked up in the PATH the PROGRAM will see when `env` sets
 * one (CreateProcessW would search the host's). Anything else is left to
 * CreateProcessW, as before. Takes ownership of `a0`; returns what to start (NULL when
 * out of memory). */
static wchar_t *win_resolve_arg0(wchar_t *a0, const wchar_t *wdir, const wchar_t *path)
{
    int sep = wcspbrk(a0, L"\\/") != NULL;
    int absolute = a0[0] == L'\\' || a0[0] == L'/' || (a0[0] && a0[1] == L':');
    if (sep && !absolute && wdir && wdir[0]) {
        size_t ld = wcslen(wdir), la = wcslen(a0);
        wchar_t *j = (wchar_t *)gptps_malloc((ld + la + 2) * sizeof *j);
        if (!j) { gptps_free(a0); return NULL; }
        memcpy(j, wdir, ld * sizeof *j);
        if (ld && wdir[ld - 1] != L'\\' && wdir[ld - 1] != L'/') j[ld++] = L'\\';
        memcpy(j + ld, a0, (la + 1) * sizeof *j);
        gptps_free(a0);
        return j;
    }
    if (!sep && path) {
        wchar_t found[MAX_PATH * 2];
        DWORD n = SearchPathW(path, a0, L".exe", (DWORD)(sizeof found / sizeof found[0]), found, NULL);
        if (n > 0 && n < sizeof found / sizeof found[0]) {
            wchar_t *r = (wchar_t *)gptps_malloc((n + 1) * sizeof *r);
            if (!r) { gptps_free(a0); return NULL; }
            memcpy(r, found, (n + 1) * sizeof *r);
            gptps_free(a0);
            return r;
        }
    }
    return a0;
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

/* `oversize` is read by the waiting thread while the reader runs, hence a LONG under
 * Interlocked*. `graceful`: the job has a stop grace (stop_grace_ms), so output past
 * the cap does not kill it here - the reader raises `oversize`, keeps draining (and
 * dropping) what the program writes, so it cannot block on a full pipe during its
 * clean-up, and the waiting thread stops the job the way it stops a timed-out one. */
typedef struct { HANDLE h; char *buf; size_t len, cap; int nomem, ioerr;
                 volatile LONG oversize; int graceful;
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

/* ABI 2.7: the program's stderr, when its tail is kept - everything is read, only
 * the last `cap` bytes stay, so a chatty program never blocks on a full pipe. */
typedef struct { HANDLE h; char *buf; size_t len, cap; } err_ctx;
static DWORD WINAPI err_proc(LPVOID p)
{
    err_ctx *t = (err_ctx *)p;
    char chunk[4096];
    DWORD got;
    while (ReadFile(t->h, chunk, (DWORD)sizeof chunk, &got, NULL) && got > 0) {
        size_t n = got;
        if (n >= t->cap) { memcpy(t->buf, chunk + (n - t->cap), t->cap); t->len = t->cap; continue; }
        if (t->len + n > t->cap) {
            size_t drop = t->len + n - t->cap;
            memmove(t->buf, t->buf + drop, t->len - drop);
            t->len -= drop;
        }
        memcpy(t->buf + t->len, chunk, n);
        t->len += n;
    }
    return 0;
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
        if (full) {                                                  /* >16 MiB */
            InterlockedExchange(&r->oversize, 1);
            if (!r->graceful) { reader_stop_child(r); break; }
            {   /* stopped with its grace by the waiting thread: drain until the end */
                char sink[4096];
                while (ReadFile(r->h, sink, (DWORD)sizeof sink, &got, NULL) && got > 0) { }
            }
            break;
        }
        r->len += got;
    }
    return 0;
}

/* The CPU cap (gptps_task_def.cpu_max_millicpus) as the job object's HARD CPU-rate
 * limit: Windows 8 and later throttle every process in the job to CpuRate / 10000 of
 * the whole machine. The struct and the class are spelled out here rather than taken
 * from <winnt.h>, which declares them only for _WIN32_WINNT >= 0x0602 in some SDKs -
 * and a build targeting an older Windows must still compile and run; there the call
 * simply fails and the cap is reported as not in force. Windows' I/O rate control is a
 * single figure for reads and writes together, and per volume, so it cannot keep
 * io_max_read_bps / io_max_write_bps as declared: those are not set, and the engine
 * says so rather than enforce something else under their name. 1 if the cap is set. */
typedef struct {
    DWORD ControlFlags;
    DWORD CpuRate;             /* a union in winnt.h; CpuRate is the member used */
} gptps_win_cpu_rate_info;
#define GPTPS_WIN_JOB_CPU_RATE_CLASS 15      /* JobObjectCpuRateControlInformation */
#define GPTPS_WIN_CPU_RATE_ENABLE    0x1u    /* JOB_OBJECT_CPU_RATE_CONTROL_ENABLE */
#define GPTPS_WIN_CPU_RATE_HARD_CAP  0x4u    /* JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP */
/* The machine's logical processors, every processor group's: CpuRate is a share of all
 * of them. GetSystemInfo counts only the caller's group (at most 64), which on a bigger
 * machine made the cap a multiple of what was declared. GetActiveProcessorCount is
 * Windows 7 and later, looked up at run time; before it there are no groups. */
typedef DWORD (WINAPI *apc_fn)(WORD);
#define GPTPS_WIN_ALL_PROCESSOR_GROUPS 0xffffu
static uint64_t win_machine_cpus(void)
{
    apc_fn apc = (apc_fn)(void (*)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                        "GetActiveProcessorCount");
    DWORD n = apc ? apc((WORD)GPTPS_WIN_ALL_PROCESSOR_GROUPS) : 0;
    if (!n) { SYSTEM_INFO sys; GetSystemInfo(&sys); n = sys.dwNumberOfProcessors; }
    return n ? (uint64_t)n : 1u;
}
static unsigned win_cpu_rate(HANDLE job, uint64_t millicpus)
{
    gptps_win_cpu_rate_info r;
    uint64_t ncpu = win_machine_cpus(), rate;
    /* millicpus of ONE cpu, as a share of all of them in 1/10000ths, rounded up so a
     * small cap never becomes 0 - which Windows refuses */
    if (millicpus >= ncpu * 1000u) rate = 10000u;   /* the whole machine, or more: and no overflow below */
    else                           rate = (millicpus * 10u + ncpu - 1u) / ncpu;
    if (rate < 1u)     rate = 1u;
    if (rate > 10000u) rate = 10000u;
    memset(&r, 0, sizeof r);
    r.ControlFlags = GPTPS_WIN_CPU_RATE_ENABLE | GPTPS_WIN_CPU_RATE_HARD_CAP;
    r.CpuRate      = (DWORD)rate;
    return SetInformationJobObject(job, (JOBOBJECTINFOCLASS)GPTPS_WIN_JOB_CPU_RATE_CLASS, &r, sizeof r) ? 1u : 0u;
}

/* ABI 2.8: the I/O caps through the job object's I/O rate control (Windows 10 1607 /
 * Server 2016 and later, looked up at run time so an older system still runs the job,
 * uncapped and said so). Windows limits reads and writes TOGETHER, per volume - so the
 * one figure set on every volume is the smaller of the two caps: neither is exceeded.
 * The struct is spelled out (JOBOBJECT_IO_RATE_CONTROL_INFORMATION), as the CPU one is.
 * 1 if at least one volume took it. */
typedef struct {
    LONG64  MaxIops;
    LONG64  MaxBandwidth;
    LONG64  ReservationIops;
    PCWSTR  VolumeName;
    ULONG   BaseIoSize;
    ULONG   ControlFlags;
} gptps_win_io_rate_info;
typedef DWORD (WINAPI *io_rate_fn)(HANDLE, gptps_win_io_rate_info *);
#define GPTPS_WIN_IO_RATE_ENABLE 0x1u          /* JOB_OBJECT_IO_RATE_CONTROL_ENABLE */
static unsigned win_io_rate(HANDLE job, uint64_t bps)
{
    io_rate_fn set = (io_rate_fn)(void (*)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                "SetIoRateControlInformationJobObject");
    wchar_t vol[MAX_PATH + 1], dev[MAX_PATH + 1];
    HANDLE it;
    unsigned ok = 0;
    if (!set || !bps) return 0;
    it = FindFirstVolumeW(vol, MAX_PATH);
    if (it == INVALID_HANDLE_VALUE) return 0;
    do {
        /* "\\?\Volume{guid}\" -> "Volume{guid}" -> its NT device name, which the
         * rate control is keyed by */
        size_t n = wcslen(vol);
        gptps_win_io_rate_info r;
        if (n < 5 || wcsncmp(vol, L"\\\\?\\", 4) != 0) continue;
        if (vol[n - 1] == L'\\') vol[n - 1] = 0;
        if (!QueryDosDeviceW(vol + 4, dev, MAX_PATH)) continue;
        memset(&r, 0, sizeof r);
        r.MaxBandwidth = (LONG64)(bps > (uint64_t)0x7FFFFFFFFFFFFFFFull ? 0x7FFFFFFFFFFFFFFFull : bps);
        r.VolumeName = dev;
        r.ControlFlags = GPTPS_WIN_IO_RATE_ENABLE;
        if (set(job, &r)) ok = 1;
    } while (FindNextVolumeW(it, vol, MAX_PATH));
    FindVolumeClose(it);
    return ok;
}

/* The launch lock: one lock for the process-wide state a launch reads or a stop
 * changes - which handles are inheritable, and which console the host is attached to.
 * Held from making the child's pipe ends inheritable to making them not inheritable
 * again (CreateProcessW in between), and by win_ctrl_break for the time the host is
 * attached to a job's console. Without it, one worker's CreateProcessW copied another
 * job's pipe ends into its program (that job then waited for its output to end for as
 * long as the other program lived), and a launch that probed the console while a stop
 * had the host attached to another job's took that console for the host's own. A spin
 * lock with a yield, as nothing here may need initialising: what it covers is a few
 * system calls. */
static volatile LONG g_launch_lock;
static void win_launch_lock(void)
{
    unsigned spins = 0;
    while (InterlockedCompareExchange(&g_launch_lock, 1, 0) != 0) Sleep(++spins < 64 ? 0 : 1);
}
static void win_launch_unlock(void) { InterlockedExchange(&g_launch_lock, 0); }

/* PROC_THREAD_ATTRIBUTE_HANDLE_LIST (Vista and later): the child inherits exactly the
 * handles listed - its three standard handles - and none of the host's other
 * inheritable ones, whoever made them inheritable. Looked up at run time and spelled
 * out here, so a build for an older Windows still compiles and runs: without it the
 * launch lock alone keeps the jobs' pipe ends apart. */
typedef BOOL (WINAPI *ptal_init_fn)(void *, DWORD, DWORD, SIZE_T *);
typedef BOOL (WINAPI *ptal_update_fn)(void *, DWORD, DWORD_PTR, void *, SIZE_T, void *, SIZE_T *);
typedef void (WINAPI *ptal_delete_fn)(void *);
typedef struct { STARTUPINFOW si; void *attrs; } gptps_win_siex;      /* STARTUPINFOEXW */
#define GPTPS_WIN_EXTENDED_STARTUPINFO 0x00080000u   /* EXTENDED_STARTUPINFO_PRESENT */
#define GPTPS_WIN_ATTR_HANDLE_LIST     0x00020002u   /* PROC_THREAD_ATTRIBUTE_HANDLE_LIST */

/* A handle the child may be given in its handle list: a real one. Before Windows 8 a
 * console handle is a pseudo handle (low two bits set), which the list refuses and the
 * console passes on by itself. */
static int win_listable(HANDLE h)
{
    return h && h != INVALID_HANDLE_VALUE && ((ULONG_PTR)h & 3u) != 3u;
}

/* CreateProcessW with stdin/stdout/stderr = in/out/err and nothing else inherited.
 * The three are not inheritable when they come in, and are not when this returns;
 * they are only for the launch, under the launch lock. `err` NULL gives the program
 * the host's own stderr, as an inheritable copy so the host's handle is not changed.
 * *host_console (when probe is set) is whether the host has a console at the moment of
 * the launch - read under the lock, so never a console a stop has borrowed. FALSE with
 * *err_out set when it was not started. */
static BOOL win_launch(wchar_t *wcmd, DWORD flags, int probe, int *host_console, void *wenv,
                       const wchar_t *wdir, HANDLE in, HANDLE out, HANDLE err,
                       PROCESS_INFORMATION *pi, DWORD *err_out)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    ptal_init_fn   init = k ? (ptal_init_fn)(void (*)(void))GetProcAddress(k, "InitializeProcThreadAttributeList") : NULL;
    ptal_update_fn upd  = k ? (ptal_update_fn)(void (*)(void))GetProcAddress(k, "UpdateProcThreadAttribute") : NULL;
    ptal_delete_fn del  = k ? (ptal_delete_fn)(void (*)(void))GetProcAddress(k, "DeleteProcThreadAttributeList") : NULL;
    gptps_win_siex sx;
    HANDLE list[3], err_dup = NULL;
    DWORD n = 0, i, one;
    SIZE_T asz = 0;
    void *attrs = NULL;
    BOOL ok;

    list[n++] = in; list[n++] = out;
    if (err) {
        list[n++] = err;                               /* the job's own stderr pipe */
    } else {
        /* The host's stderr. Its own handle is never touched: a copy is listed (and
         * closed below). One that cannot be copied - a pseudo console handle - is passed
         * as it is, which is what the console expects. */
        err = GetStdHandle(STD_ERROR_HANDLE);
        if (win_listable(err) && DuplicateHandle(GetCurrentProcess(), err, GetCurrentProcess(), &err_dup,
                                                 0, FALSE, DUPLICATE_SAME_ACCESS))
            list[n++] = err = err_dup;
    }

    memset(&sx, 0, sizeof sx);
    sx.si.cb = sizeof sx.si;
    sx.si.dwFlags = STARTF_USESTDHANDLES;
    sx.si.hStdInput = in; sx.si.hStdOutput = out; sx.si.hStdError = err;
    if (init && upd && del) {
        (void)init(NULL, 1, 0, &asz);                  /* fails, with the size it needs */
        if (asz && (attrs = gptps_malloc(asz)) != NULL) {
            if (!init(attrs, 1, 0, &asz)) { gptps_free(attrs); attrs = NULL; }
            else if (!upd(attrs, 0, (DWORD_PTR)GPTPS_WIN_ATTR_HANDLE_LIST, list, n * sizeof list[0], NULL, NULL)) {
                del(attrs); gptps_free(attrs); attrs = NULL;
            }
        }
    }
    if (attrs) { sx.si.cb = sizeof sx; sx.attrs = attrs; flags |= GPTPS_WIN_EXTENDED_STARTUPINFO; }

    win_launch_lock();
    for (i = 0; i < n; ++i) SetHandleInformation(list[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (probe) {
        *host_console = GetConsoleProcessList(&one, 1) != 0;   /* 0: attached to no console */
        if (!*host_console) flags |= CREATE_NO_WINDOW;
    } else {
        flags |= CREATE_NO_WINDOW;
    }
    ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, flags, wenv, wdir, &sx.si, pi);
    *err_out = ok ? 0 : GetLastError();
    for (i = 0; i < n; ++i) SetHandleInformation(list[i], HANDLE_FLAG_INHERIT, 0);
    win_launch_unlock();

    if (attrs) { del(attrs); gptps_free(attrs); }
    if (err_dup) CloseHandle(err_dup);
    return ok;
}

/* ABI 2.8: Ctrl+Break to a job's process group, whether or not the HOST has a console.
 * With one, the job shares it and the signal is sent from here. Without one (a
 * service), the job runs in a hidden console of its own; the host attaches to it just
 * long enough to send the signal - to the job's group only, which the host is not in -
 * and detaches. That attachment is process-wide state, so it is taken under the launch
 * lock and only while the host has no console of its own, and the standard handles it
 * can replace are put back. 1 if the signal was sent. */
static int win_ctrl_break(DWORD pid, int host_console)
{
    HANDLE h_in, h_out, h_err;
    DWORD one;
    int sent = 0;
    if (host_console) return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid) != 0;
    win_launch_lock();
    h_in = GetStdHandle(STD_INPUT_HANDLE); h_out = GetStdHandle(STD_OUTPUT_HANDLE); h_err = GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleProcessList(&one, 1) == 0 && AttachConsole(pid)) {
        sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid) != 0;
        FreeConsole();
    }
    SetStdHandle(STD_INPUT_HANDLE, h_in); SetStdHandle(STD_OUTPUT_HANDLE, h_out); SetStdHandle(STD_ERROR_HANDLE, h_err);
    win_launch_unlock();
    return sent;
}

/* Is anything of the job still running? The whole job - a launcher that exits at once
 * must not cost its workers their clean-up - when the program is in the job object;
 * else the program itself. */
static int win_job_alive(HANDLE job, int assigned, HANDLE proc)
{
    if (assigned) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION a;
        if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &a, sizeof a, NULL))
            return a.ActiveProcesses != 0;
    }
    return WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
}

/* The Win32 error as UTF-8 text, for the start-failure WARN. */
static void win_errtext(DWORD err, char *buf, size_t n)
{
    wchar_t w[256];
    DWORD k;
    if (!n) return;
    buf[0] = 0;
    k = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, w,
                       (DWORD)(sizeof w / sizeof w[0]), NULL);
    while (k && (w[k - 1] == L'\r' || w[k - 1] == L'\n' || w[k - 1] == L' ' || w[k - 1] == L'.')) w[--k] = 0;
    if (!k || !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, NULL, NULL)) snprintf(buf, n, "error %lu", (unsigned long)err);
}

/* One WARN per process for a job object that could not be made or the program put in
 * (the engine's own per-task-type line names what was asked for and did not hold). */
static volatile LONG g_job_warned;
static void win_job_warn(const gptps_task_def *def, const char *step, DWORD err, int in_job)
{
    char msg[640], txt[160];
    if (InterlockedExchange(&g_job_warned, 1) != 0) return;
    win_errtext(err, txt, sizeof txt);
    snprintf(msg, sizeof msg, "task '%s': its program could not be put in a kill-on-close job object "
             "(%s failed: %s, error %lu), so for it %s. Said once; later jobs may be in the same state",
             (def && def->name) ? def->name : "?", step, txt, (unsigned long)err,
             in_job ? "neither the memory cap nor GPTPS_TASK_DIE_WITH_HOST holds"
                    : "no memory, CPU or I/O cap holds, nor GPTPS_TASK_DIE_WITH_HOST, and a stop ends "
                      "only the program itself, not what it started");
    gptps_log(NULL, GPTPS_LOG_WARN, msg);
}

gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   gptps_exec_caps *caps, uint32_t timeout_s, const uint32_t *cancel,
                                   void **out_result, size_t *out_len, gptps_exec_meter *meter)
{
    const uint64_t mem_cap = caps->mem;
    unsigned cpu_set = 0;              /* the job object took the CPU-rate cap */
    unsigned io_set = 0;               /* ABI 2.8: ...and the I/O-rate cap */
    int limits_set = 0;                /* ...and its limits: kill-on-close, the memory cap */
    const char *job_step = NULL;       /* what failed of the job object, if anything did */
    DWORD job_err = 0;
    const char *const *argv = def ? def->argv : NULL;
    SECURITY_ATTRIBUTES sa;
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL, job = NULL, wt = NULL, rt = NULL;
    HANDLE errR = NULL, errW = NULL, et = NULL;   /* ABI 2.7: stderr, when its tail is kept */
    err_ctx ec;
    PROCESS_INFORMATION pi;
    wchar_t *wcmd = NULL, *wdir = NULL, *wenv = NULL;   /* ABI 2.7: CreateProcessW's */
    wchar_t *wpath = NULL;                              /* ABI 2.8: the PATH `env` sets, if any */
    int ctrl_break = 0;                                 /* ABI 2.7: stop with Ctrl+Break first */
    int host_console = 0;                               /* ABI 2.8: ...sent from the host's console */
    writer_ctx wc;
    reader_ctx rc;
    DWORD code = 1, waited, start_err = 0;
    int killed = 0, assigned = 0, graceful_stop = 0;
    gptps_status eff;
    gptps_status kill_st = GPTPS_E_TIMEOUT;   /* why we killed the child, if we did */

    /* def->child_setup is a POSIX fork-time hook; Windows has no fork model, so
     * it does not apply to CreateProcess and is intentionally ignored here. */
    *out_result = NULL; *out_len = 0;
    if (!argv || !argv[0]) return GPTPS_E_INVAL;

    /* Every pipe end is made NOT inheritable: the child's ends become inheritable only
     * for its own launch (win_launch), so no other program started meanwhile - by
     * another worker, for another job - can be handed one. A program holding another
     * job's stdout write end kept that job's output from ever ending. */
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = FALSE;
    if (!CreatePipe(&inR, &inW, &sa, 0)) return GPTPS_E_IO;
    if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW); return GPTPS_E_IO; }
    memset(&ec, 0, sizeof ec);
    if (meter && meter->err_cap) {
        ec.cap = meter->err_cap;
        ec.buf = (char *)gptps_malloc(ec.cap);
        if (!ec.buf || !CreatePipe(&errR, &errW, &sa, 0)) {
            gptps_status why = ec.buf ? GPTPS_E_IO : GPTPS_E_NOMEM;
            gptps_free(ec.buf);
            CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
            return why;
        }
    }

    job = CreateJobObjectA(NULL, NULL);
    if (!job) { job_step = "CreateJobObject"; job_err = GetLastError(); }
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof jeli);
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (mem_cap >= GPTPS_WIN_MEMCAP_FLOOR) {
            jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            jeli.ProcessMemoryLimit = (SIZE_T)mem_cap;
        }
        limits_set = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof jeli) != 0;
        if (!limits_set) { job_step = "SetInformationJobObject"; job_err = GetLastError(); }
        if (caps->cpu_millicpus) cpu_set = win_cpu_rate(job, caps->cpu_millicpus);
        if (caps->io_rbps || caps->io_wbps) {
            uint64_t one = caps->io_rbps && caps->io_wbps ? (caps->io_rbps < caps->io_wbps ? caps->io_rbps : caps->io_wbps)
                                                          : (caps->io_rbps ? caps->io_rbps : caps->io_wbps);
            io_set = win_io_rate(job, one);
        }
    }

    {   /* each argument to UTF-16 on its own (one in the ANSI code page does not drag the
         * rest with it), argv[0] found the POSIX way, then quoted into one line */
        size_t n = 0, i;
        wchar_t **wv;
        int ok = 1;
        while (argv[n]) ++n;
        wv = (wchar_t **)gptps_calloc(n + 1, sizeof *wv);
        if (!wv) ok = 0;
        if (ok && def->work_dir && !(wdir = win_wide(def->work_dir))) ok = 0;
        if (ok && def->env && !(wenv = win_env_block(def->env, &wpath))) ok = 0;
        for (i = 0; ok && i < n; ++i) if (!(wv[i] = win_wide(argv[i]))) ok = 0;
        if (ok && !(wv[0] = win_resolve_arg0(wv[0], wdir, wpath))) ok = 0;
        if (ok) wcmd = build_cmdline_w(wv, n);
        if (wv) { for (i = 0; i < n; ++i) gptps_free(wv[i]); gptps_free(wv); }
        gptps_free(wpath);
    }
    if (!wcmd) { CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
                 if (errR) { CloseHandle(errR); CloseHandle(errW); }
                 gptps_free(ec.buf);
                 if (job) CloseHandle(job);
                 gptps_free(wdir); gptps_free(wenv);
                 return GPTPS_E_NOMEM; }

    memset(&pi, 0, sizeof pi);
    /* stop_grace_ms: Windows' SIGTERM is Ctrl+Break, sent to the program's own process
     * group (CREATE_NEW_PROCESS_GROUP), which the host is never in. A host with a
     * console shares it with the program; one without (a service) gives the program a
     * hidden console of its own, and win_ctrl_break reaches it there. */
    ctrl_break = caps->stop_grace_ms != 0;
    if (!win_launch(wcmd, CREATE_SUSPENDED
                          | (ctrl_break ? CREATE_NEW_PROCESS_GROUP : 0)
                          | (wenv ? CREATE_UNICODE_ENVIRONMENT : 0),
                    ctrl_break, &host_console, wenv, wdir, inR, outW, errW, &pi, &start_err)) {
        /* ABI 2.8: it never started - say why, for the event, the dead letter and once
         * in the log. ERROR_DIRECTORY is work_dir's; anything else the program's. */
        DWORD err = start_err;
        caps->start_what = (err == ERROR_DIRECTORY && wdir) ? GPTPS_START_WORKDIR : GPTPS_START_EXEC;
        caps->start_err = (int)err;
        win_errtext(err, caps->start_msg, sizeof caps->start_msg);
        gptps_meter_put(meter, GPTPS_M_START_ERROR, (uint64_t)err, GPTPS_UNIT_CODE, GPTPS_MEASURE_CURRENT,
                        "win.createprocess");
        gptps_free(wcmd); gptps_free(wdir); gptps_free(wenv);
        if (errR) { CloseHandle(errR); CloseHandle(errW); }
        gptps_free(ec.buf);
        CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
        if (job) CloseHandle(job);
        return GPTPS_E_TASK; /* program could not be started (or work_dir entered) */
    }
    gptps_free(wcmd); gptps_free(wdir); gptps_free(wenv);
    CloseHandle(inR); CloseHandle(outW); /* child owns these; parent keeps inW + outR */
    if (errW) { CloseHandle(errW); errW = NULL; }

    if (job) {
        if (AssignProcessToJobObject(job, pi.hProcess)) assigned = 1;
        else if (!job_step) { job_step = "AssignProcessToJobObject"; job_err = GetLastError(); }
    }
    if (job_step) win_job_warn(def, job_step, job_err, assigned);
    /* A cap on a job the program is not in holds nothing. The I/O caps are never set
     * here (see win_cpu_rate), so the engine reports them as not in force. Dying with
     * the host is the job object's kill-on-close: it holds only with the limit set and
     * the program in the job - the engine says so for a task that asked, when not. */
    caps->applied = (assigned && cpu_set) ? GPTPS_CAP_CPU : 0u;
    if (assigned && io_set) caps->applied |= GPTPS_CAP_IO | GPTPS_CAP_IO_ONE;
    if (ctrl_break) caps->applied |= GPTPS_CAP_GRACE;
    if (assigned && limits_set) caps->applied |= GPTPS_CAP_DIE;   /* dies with its last handle - the host's */

    /* The helper threads, started while the program is still suspended: a program
     * nobody reads, or that is handed an empty stdin in place of its payload, must not
     * run at all - one that exits 0 on that is a success with the wrong result. Without
     * them the attempt fails with GPTPS_E_NOMEM (a thread is memory), as other resource
     * failures here do. No payload needs no writer: stdin is closed at once. */
    wc.h = inW; wc.data = (const char *)payload; wc.len = plen; wc.ioerr = 0;
    wc.proc = pi.hProcess; wc.job = job; wc.assigned = assigned;
    rc.h = outR; rc.buf = NULL; rc.len = rc.cap = 0; rc.nomem = rc.ioerr = 0; rc.oversize = 0;
    rc.graceful = ctrl_break;
    rc.proc = pi.hProcess; rc.job = job; rc.assigned = assigned;
    if (plen) wt = CreateThread(NULL, 0, writer_proc, &wc, 0, NULL);
    if (!wt) CloseHandle(inW);                 /* stdin's EOF, now */
    rt = CreateThread(NULL, 0, reader_proc, &rc, 0, NULL);
    if (errR) {
        ec.h = errR;
        et = CreateThread(NULL, 0, err_proc, &ec, 0, NULL);
        if (!et) { CloseHandle(errR); errR = NULL; }
    }
    if ((plen && !wt) || !rt || (ec.cap && !et)) {
        killed = 1; kill_st = GPTPS_E_NOMEM;   /* never resumed: it never ran */
        if (assigned) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
    } else {
        ResumeThread(pi.hThread);
    }

    /* Wait for the child in bounded slices so a raised cancel flag (gptps_cancel /
     * shutdown / task removal) OR the deadline hard-kills it - including when
     * timeout_s==0 (no deadline), which otherwise waited forever and could not be
     * cancelled. The writer/reader threads run concurrently, so there is no stdin/
     * stdout deadlock to solve here (unlike the POSIX single-thread pump). */
    {
        uint64_t deadline = timeout_s ? gptps_hal_monotonic_ms() + (uint64_t)timeout_s * 1000u : 0;
        win_sampler sm;
        win_sampler_init(&sm, meter);
        while (!killed) {                     /* killed already: its threads could not start */
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
            /* stdout past the result cap, in a job with a stop grace: stopped as a
             * timed-out one is, with its grace - it may be in the middle of its work -
             * and the attempt fails with GPTPS_E_IO, as an ungraceful one's does */
            if (InterlockedCompareExchange(&rc.oversize, 0, 0)) {
                killed = 1; kill_st = GPTPS_E_IO; graceful_stop = 1; break;
            }
            /* WAIT_TIMEOUT: slice elapsed, loop and re-check deadline/cancel */
        }
        /* A grace to give (timeout, cancel, too much output - not our own failure):
         * Ctrl+Break to the program's group, then up to the grace for the WHOLE job to
         * exit before the kill. Its output threads keep reading meanwhile, so it cannot
         * block on us. */
        if (killed && (kill_st == GPTPS_E_TIMEOUT || kill_st == GPTPS_E_CANCELLED)) graceful_stop = 1;
        if (killed && ctrl_break && graceful_stop) {
            if (win_ctrl_break(pi.dwProcessId, host_console)) {
                uint64_t until = gptps_hal_monotonic_ms() + caps->stop_grace_ms;
                while (win_job_alive(job, assigned, pi.hProcess) && gptps_hal_monotonic_ms() < until)
                    Sleep(10);
            } else {
                caps->applied &= ~GPTPS_CAP_GRACE;   /* could not be sent: the engine says so, once */
            }
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
    if (et) {                                  /* the same bounded join, for stderr */
        if (WaitForSingleObject(et, GPTPS_WIN_JOIN_GRACE_MS) == WAIT_TIMEOUT) GPTPS_WIN_CANCEL_SYNC_IO(et);
        CloseHandle(errR);
        errR = NULL;
        WaitForSingleObject(et, INFINITE); CloseHandle(et);
    }
    if (errR) CloseHandle(errR);               /* no stderr thread was started */
    if (meter && ec.len) { meter->err = ec.buf; meter->err_len = ec.len; }
    else gptps_free(ec.buf);
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
    if (rc.oversize) caps->out_capped = GPTPS_WIN_RESULT_CAP;   /* the engine says why, once */
    /* ABI 2.7: how it ended, when it ended on its own - every branch above but the last
     * may have terminated it, and then its code is ours, not the program's */
    if (!killed && !rc.oversize && !wc.ioerr && !rc.ioerr && !rc.nomem)
        gptps_meter_put(meter, GPTPS_M_EXIT_CODE, (uint64_t)code, GPTPS_UNIT_CODE,
                        GPTPS_MEASURE_CURRENT, "win.exitcode");
    (void)io_set;

    if (eff == GPTPS_OK) { *out_result = rc.buf; *out_len = rc.len; }
    else gptps_free(rc.buf);
    return eff;
}

#endif /* _WIN32 */
