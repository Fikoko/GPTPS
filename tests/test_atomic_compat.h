/*
 * test_atomic_compat.h - portability shim for the TEST/EXAMPLE suite only.
 *
 * The tests use a handful of GCC/Clang __atomic_* builtins for cross-thread
 * counters (worker threads bump them; the main thread reads them). MSVC has no
 * __atomic_* builtins, so under MSVC we map the exact forms the suite uses onto
 * the Win32 Interlocked* intrinsics. Every counter the suite touches is a plain
 * `int`, which is a 32-bit LONG on Windows, so a single width suffices.
 *
 * On GCC/Clang this header is empty: the real builtins are used unchanged, so the
 * POSIX builds are byte-for-byte identical. Wired in MSVC-only via CMake (/FI).
 *
 * Test-only: NOT part of the library or its public ABI. The Interlocked* calls
 * are full barriers, so the ignored memory-order arguments are safe to drop.
 */
#ifndef GPTPS_TEST_ATOMIC_COMPAT_H
#define GPTPS_TEST_ATOMIC_COMPAT_H

#if defined(_MSC_VER)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef __ATOMIC_SEQ_CST
#define __ATOMIC_SEQ_CST 5   /* ignored: Interlocked* are sequentially consistent */
#endif

/* *p += n, return NEW value */
#define __atomic_add_fetch(p, n, mo) \
    ((int)(InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(n)) + (LONG)(n)))
/* *p -= n, return NEW value */
#define __atomic_sub_fetch(p, n, mo) \
    ((int)(InterlockedExchangeAdd((volatile LONG *)(p), -(LONG)(n)) - (LONG)(n)))
/* *p += n, return OLD value */
#define __atomic_fetch_add(p, n, mo) \
    ((int)InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(n)))
/* return *p (CompareExchange with comparand==exchange==0 is a barriered load) */
#define __atomic_load_n(p, mo) \
    ((int)InterlockedCompareExchange((volatile LONG *)(p), 0, 0))
/* *p = v */
#define __atomic_store_n(p, v, mo) \
    ((void)InterlockedExchange((volatile LONG *)(p), (LONG)(v)))

/* bool CAS: if *p==*expp -> *p=des, return 1; else *expp=*p, return 0 */
static __inline int gptps_test_cas_int(volatile LONG *p, LONG *expp, LONG des)
{
    LONG old = InterlockedCompareExchange(p, des, *expp);
    if (old == *expp) return 1;
    *expp = old;
    return 0;
}
#define __atomic_compare_exchange_n(p, expp, des, weak, smo, fmo) \
    gptps_test_cas_int((volatile LONG *)(p), (LONG *)(expp), (LONG)(des))

#endif /* _MSC_VER */

#endif /* GPTPS_TEST_ATOMIC_COMPAT_H */
