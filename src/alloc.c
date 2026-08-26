/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * alloc.c - the core allocator seam.
 *
 * Every core allocation routes through gptps_malloc/calloc/realloc/free, which
 * default to the C library but can be redirected process-wide via
 * gptps_set_allocator(). This lets a host with a static pool / no libc heap (an
 * embedded or bare-metal target) supply its own memory, the same way SQLite's
 * sqlite3_config(SQLITE_CONFIG_MALLOC, ...) works.
 *
 * SCOPE: this covers the portable CORE (engine, settings, config, executors).
 * The HAL (hal_posix.c / hal_win.c) deliberately uses libc directly - it is the
 * platform seam you replace wholesale on an exotic target, and it manages its
 * own thread/sync structs there.
 *
 * THREADING: install the allocator ONCE, before the first gptps_open, and do not
 * change it while engines exist. Reads are unsynchronized by design (the override
 * is configuration, not runtime state).
 */
#include "gptps.h"
#include "gptps_internal.h"

#include <stdlib.h>
#include <string.h>

static gptps_allocator g_alloc;   /* all-zero until set */
static int             g_have;    /* 0 => use the C library */

gptps_status gptps_set_allocator(const gptps_allocator *a)
{
    if (!a) {                                  /* reset to libc */
        memset(&g_alloc, 0, sizeof g_alloc);
        g_have = 0;
        return GPTPS_OK;
    }
    if (a->struct_size < GPTPS_ALLOCATOR_MIN_SIZE) return GPTPS_E_INVAL; /* ABI: append-safe floor */
    if (!a->malloc_fn || !a->realloc_fn || !a->free_fn) return GPTPS_E_INVAL;
    g_alloc = *a;
    g_have  = 1;
    return GPTPS_OK;
}

void *gptps_malloc(size_t size)
{
    return g_have ? g_alloc.malloc_fn(size, g_alloc.user_data) : malloc(size);
}

void *gptps_realloc(void *ptr, size_t size)
{
    /* Route a NULL ptr to malloc_fn rather than forwarding it, so realloc_fn is
     * never handed one - the same courtesy free_fn already gets below. The core
     * uses realloc-as-malloc for every growable buffer (the TOML table and its
     * arrays, the resource table, the per-item resource snapshot, both executors'
     * capture buffers), so a first growth ALWAYS passes NULL. A host reading the
     * header's explicit "free_fn needn't handle NULL" reasonably infers the same of
     * realloc_fn, and a pool allocator written on that assumption dereferences
     * NULL - HDR and crashes inside the host's own code. Behaviour-neutral on the
     * libc path: realloc(NULL, n) is defined as malloc(n). */
    if (!ptr) return gptps_malloc(size);
    return g_have ? g_alloc.realloc_fn(ptr, size, g_alloc.user_data) : realloc(ptr, size);
}

void gptps_free(void *ptr)
{
    if (!ptr) return;                          /* custom free_fn needn't handle NULL */
    if (g_have) g_alloc.free_fn(ptr, g_alloc.user_data);
    else        free(ptr);
}

void *gptps_calloc(size_t n, size_t size)
{
    size_t total;
    void  *p;
    if (!g_have) return calloc(n, size);       /* libc: overflow-safe, zero-fills */
    if (n != 0 && size > (size_t)-1 / n) return NULL;   /* overflow guard */
    total = n * size;
    p = g_alloc.malloc_fn(total ? total : 1, g_alloc.user_data);
    if (p) memset(p, 0, total);
    return p;
}
