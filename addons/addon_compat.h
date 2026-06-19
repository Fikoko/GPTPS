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

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
typedef CRITICAL_SECTION apx_mutex;
static void apx_mutex_init(apx_mutex *m)    { InitializeCriticalSection(m); }
static void apx_mutex_lock(apx_mutex *m)    { EnterCriticalSection(m); }
static void apx_mutex_unlock(apx_mutex *m)  { LeaveCriticalSection(m); }
static void apx_mutex_destroy(apx_mutex *m) { DeleteCriticalSection(m); }
static int  apx_fsync(FILE *f)              { return _commit(_fileno(f)); }
#else
#  include <pthread.h>
#  include <unistd.h>
typedef pthread_mutex_t apx_mutex;
static void apx_mutex_init(apx_mutex *m)    { pthread_mutex_init(m, NULL); }
static void apx_mutex_lock(apx_mutex *m)    { pthread_mutex_lock(m); }
static void apx_mutex_unlock(apx_mutex *m)  { pthread_mutex_unlock(m); }
static void apx_mutex_destroy(apx_mutex *m) { pthread_mutex_destroy(m); }
static int  apx_fsync(FILE *f)              { return fsync(fileno(f)); }
#endif

/* silence -Wunused-function for whichever primitive a given add-on doesn't use */
static void apx_compat_keep_refs(void)
{
    (void)apx_mutex_init; (void)apx_mutex_lock; (void)apx_mutex_unlock;
    (void)apx_mutex_destroy; (void)apx_fsync; (void)apx_compat_keep_refs;
}

#endif /* GPTPS_ADDON_COMPAT_H */
