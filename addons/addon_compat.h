/*
 * addon_compat.h - tiny portable primitives shared by the bundled add-ons, so
 * they build and run on POSIX and Windows from the same source. Just a mutex and
 * a file-sync; everything else in the add-ons is already the public C99 API.
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
static APX_UNUSED int  apx_fsync(FILE *f)              { return _commit(_fileno(f)); }
/* NTFS has no durable directory-entry fsync API exposed here; rename is
 * effectively durable once the file data is committed, so this is a no-op. */
static APX_UNUSED int  apx_dir_fsync(const char *dir)  { (void)dir; return 0; }
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <fcntl.h>
typedef pthread_mutex_t apx_mutex;
static APX_UNUSED void apx_mutex_init(apx_mutex *m)    { pthread_mutex_init(m, NULL); }
static APX_UNUSED void apx_mutex_lock(apx_mutex *m)    { pthread_mutex_lock(m); }
static APX_UNUSED void apx_mutex_unlock(apx_mutex *m)  { pthread_mutex_unlock(m); }
static APX_UNUSED void apx_mutex_destroy(apx_mutex *m) { pthread_mutex_destroy(m); }
static APX_UNUSED int  apx_fsync(FILE *f)              { return fsync(fileno(f)); }
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
