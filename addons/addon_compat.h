/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_compat.h - tiny portable primitives shared by the bundled add-ons, so
 * they build and run on POSIX and Windows from the same source. A mutex, a
 * condition variable with a timed wait, and a file-sync; everything else in the
 * add-ons is already the public C99 API.
 *
 * NOT the HAL. gptps_hal.h is INTERNAL to the core and deliberately not installed,
 * and an add-on distributed as a binary plugin cannot link core symbols at all - so
 * exporting the HAL to remove this small duplication would create a second public
 * ABI surface beside the one the project has declared permanent. The duplication is
 * the correct trade; do not "clean it up".
 *
 * Include AFTER any _POSIX_C_SOURCE define. Functions are static (header-local).
 */
#ifndef GPTPS_ADDON_COMPAT_H
#define GPTPS_ADDON_COMPAT_H

#include <stdio.h>

/* a given add-on may use only some of these; mark them so -Wall stays quiet */
#if defined(__GNUC__)
#  define APX_UNUSED __attribute__((unused))
#else
#  define APX_UNUSED
#endif

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
typedef CRITICAL_SECTION apx_mutex;
static APX_UNUSED void apx_mutex_init(apx_mutex *m)    { InitializeCriticalSection(m); }
static APX_UNUSED void apx_mutex_lock(apx_mutex *m)    { EnterCriticalSection(m); }
static APX_UNUSED void apx_mutex_unlock(apx_mutex *m)  { LeaveCriticalSection(m); }
static APX_UNUSED void apx_mutex_destroy(apx_mutex *m) { DeleteCriticalSection(m); }
typedef CONDITION_VARIABLE apx_cond;
static APX_UNUSED void apx_cond_init(apx_cond *c)      { InitializeConditionVariable(c); }
static APX_UNUSED void apx_cond_signal(apx_cond *c)    { WakeConditionVariable(c); }
static APX_UNUSED void apx_cond_broadcast(apx_cond *c) { WakeAllConditionVariable(c); }
static APX_UNUSED void apx_cond_destroy(apx_cond *c)   { (void)c; }  /* no destructor on Win32 */
/* Timed wait. Returns 1 if signalled, 0 on timeout. Spurious wakeups are possible
 * on both platforms, so every caller must re-check its predicate in a loop. */
static APX_UNUSED int apx_cond_wait_ms(apx_cond *c, apx_mutex *m, unsigned ms)
{ return SleepConditionVariableCS(c, m, (DWORD)ms) ? 1 : 0; }
static APX_UNUSED int  apx_fsync(FILE *f)              { return _commit(_fileno(f)); }
static APX_UNUSED int  apx_truncate(FILE *f, long len) { return _chsize(_fileno(f), len); }
/* NTFS has no durable directory-entry fsync API exposed here; rename is
 * effectively durable once the file data is committed, so this is a no-op. */
static APX_UNUSED int  apx_dir_fsync(const char *dir)  { (void)dir; return 0; }
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <time.h>
#  include <errno.h>
typedef pthread_mutex_t apx_mutex;
static APX_UNUSED void apx_mutex_init(apx_mutex *m)    { pthread_mutex_init(m, NULL); }
static APX_UNUSED void apx_mutex_lock(apx_mutex *m)    { pthread_mutex_lock(m); }
static APX_UNUSED void apx_mutex_unlock(apx_mutex *m)  { pthread_mutex_unlock(m); }
static APX_UNUSED void apx_mutex_destroy(apx_mutex *m) { pthread_mutex_destroy(m); }
typedef pthread_cond_t apx_cond;
static APX_UNUSED void apx_cond_init(apx_cond *c)      { pthread_cond_init(c, NULL); }
static APX_UNUSED void apx_cond_signal(apx_cond *c)    { pthread_cond_signal(c); }
static APX_UNUSED void apx_cond_broadcast(apx_cond *c) { pthread_cond_broadcast(c); }
static APX_UNUSED void apx_cond_destroy(apx_cond *c)   { pthread_cond_destroy(c); }
/* Timed wait. Returns 1 if signalled, 0 on timeout. Spurious wakeups are possible
 * on both platforms, so every caller must re-check its predicate in a loop.
 *
 * Deliberately uses CLOCK_REALTIME via pthread_cond_timedwait's default clock: a
 * pthread_condattr_setclock(CLOCK_MONOTONIC) variant is not portable (macOS has no
 * such attr), and the alternative - pthread_cond_timedwait_relative_np - is
 * Apple-only. A wall-clock step therefore skews the deadline; that is a bounded,
 * documented wart on a timeout, not a correctness bug on the predicate, which every
 * caller re-checks anyway. */
static APX_UNUSED int apx_cond_wait_ms(apx_cond *c, apx_mutex *m, unsigned ms)
{
    struct timespec ts;
#if defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    ts.tv_sec = (time_t)time(NULL); ts.tv_nsec = 0;
#endif
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT ? 0 : 1;
}
static APX_UNUSED int  apx_fsync(FILE *f)              { return fsync(fileno(f)); }
static APX_UNUSED int  apx_truncate(FILE *f, long len) { return ftruncate(fileno(f), (off_t)len); }
/* fsync the directory so a rename of a journal file is durable across a crash
 * (the rename's directory-entry update must itself be flushed). */
static APX_UNUSED int  apx_dir_fsync(const char *dir)
{
    int fd, rc;
#ifdef O_DIRECTORY
    fd = open(dir, O_RDONLY | O_DIRECTORY);
#else
    fd = open(dir, O_RDONLY);
#endif
    if (fd < 0) return -1;
    rc = fsync(fd);
    close(fd);
    return rc;
}
#endif

#endif /* GPTPS_ADDON_COMPAT_H */
