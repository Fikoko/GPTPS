/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_compat.h - tiny portable primitives shared by the bundled add-ons, so
 * they build and run on POSIX and Windows from the same source. A mutex, a
 * condition variable with a timed wait, a file-sync, and whether a file exists;
 * everything else in the add-ons is already the public C99 API.
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
#  include <errno.h>
#  include <sys/types.h>
#  include <sys/stat.h>
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
/* Sync by descriptor, for a file another thread may be writing at the same time:
 * _commit holds the CRT's per-descriptor lock for the whole flush, which would stall
 * every write to the file behind it, so this flushes the OS handle directly. */
static APX_UNUSED int  apx_fileno(FILE *f)             { return _fileno(f); }
static APX_UNUSED int  apx_fsync_fd(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    return (h != INVALID_HANDLE_VALUE && FlushFileBuffers(h)) ? 0 : -1;
}
/* NTFS has no durable directory-entry fsync API exposed here; rename is
 * effectively durable once the file data is committed, so this is a no-op. */
static APX_UNUSED int  apx_dir_fsync(const char *dir)  { (void)dir; return 0; }
/* Whether `path` exists: 1 if it does (with its size and modification time, in
 * seconds), 0 if it does not, -1 if that cannot be told. A file this process may not
 * read still exists. */
static APX_UNUSED int apx_file_info(const char *path, long long *size, long long *mtime)
{
    struct _stat64 st;
    if (_stat64(path, &st) == 0) { *size = (long long)st.st_size; *mtime = (long long)st.st_mtime; return 1; }
    return errno == ENOENT ? 0 : -1;
}
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <time.h>
#  include <errno.h>
#  include <sys/types.h>
#  include <sys/stat.h>
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
/* Sync by descriptor, for a file another thread may be writing at the same time:
 * fsync needs no stdio lock, and the kernel lets writes to the file go on meanwhile.
 * On macOS fsync() hands the data to the drive without flushing the drive's own
 * cache, so a power cut can still take it; F_FULLFSYNC flushes that too, which is
 * what "durable" promises. A file system that refuses it (some network and FUSE
 * mounts) gets fsync, the best it offers. F_FULLFSYNC is a Darwin extension: the
 * includer defines _DARWIN_C_SOURCE beside _POSIX_C_SOURCE, or it is hidden and
 * this quietly falls back to fsync. */
static APX_UNUSED int  apx_fileno(FILE *f)             { return fileno(f); }
static APX_UNUSED int  apx_fsync_fd(int fd)
{
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    return fsync(fd);
}
static APX_UNUSED int  apx_fsync(FILE *f)              { return apx_fsync_fd(fileno(f)); }
static APX_UNUSED int  apx_truncate(FILE *f, long len) { return ftruncate(fileno(f), (off_t)len); }
/* An errno that says this directory cannot be synced here, rather than that a sync
 * failed: the process may write and search it but not read it, so cannot open it
 * (EACCES, EPERM), or its file system has no fsync for a directory (EINVAL, ENOTSUP,
 * EOPNOTSUPP, EROFS - fsync(2) lists EROFS beside EINVAL for that - or EBADF, EISDIR
 * where a platform reports it so). PostgreSQL and SQLite treat these the same way. */
static APX_UNUSED int apx_cannot_sync(int err)
{
    switch (err) {
    case EACCES: case EPERM: case EINVAL: case EBADF: case EISDIR: case EROFS:
#if defined(ENOTSUP)
    case ENOTSUP:
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
    case EOPNOTSUPP:
#endif
        return 1;
    default:
        return 0;
    }
}
/* fsync the directory so a rename of a journal file is durable across a crash
 * (the rename's directory-entry update must itself be flushed). Returns 0 once it
 * is - and when this directory cannot be synced at all (apx_cannot_sync): there it
 * never could be, and the caller carries on as it always has. -1, with errno set,
 * only for a sync that failed: an I/O error. */
static APX_UNUSED int  apx_dir_fsync(const char *dir)
{
    int fd, rc, err;
#ifdef O_DIRECTORY
    fd = open(dir, O_RDONLY | O_DIRECTORY);
#else
    fd = open(dir, O_RDONLY);
#endif
    if (fd < 0) return apx_cannot_sync(errno) ? 0 : -1;
    rc = fsync(fd);
    err = errno;
    close(fd);
    if (rc == 0 || apx_cannot_sync(err)) return 0;
    errno = err;
    return -1;
}
/* Whether `path` exists: 1 if it does (with its size and modification time, in
 * seconds), 0 if it does not, -1 if that cannot be told. A file this process may not
 * read still exists. */
static APX_UNUSED int apx_file_info(const char *path, long long *size, long long *mtime)
{
    struct stat st;
    if (stat(path, &st) == 0) { *size = (long long)st.st_size; *mtime = (long long)st.st_mtime; return 1; }
    return errno == ENOENT ? 0 : -1;
}
#endif

#endif /* GPTPS_ADDON_COMPAT_H */
