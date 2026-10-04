/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * driver.c - a small coverage-guided fuzzer for the harnesses in tests/fuzz, for
 * GCC, which has no libFuzzer, and for Clang alike. POSIX only.
 *
 * The code under test is compiled with -fsanitize-coverage=trace-pc,trace-cmp: the
 * compiler calls __sanitizer_cov_trace_pc in every basic block and a trace_cmp hook
 * at every integer comparison, and ASan's interceptors call the strcmp and memcmp
 * hooks below. This file must NOT be compiled with those flags: it would trace
 * itself.
 *
 *   Coverage  - each block's address is hashed, and the pair (previous block, this
 *               block) indexes a map of hit counters: AFL's edge coverage. A feature
 *               is an edge hit a number of times that falls in a power-of-two bucket
 *               no input reached before.
 *   Corpus    - an input that reaches a new feature is kept, in memory and as a file
 *               in the first directory named. Inputs are picked for mutation with a
 *               lean toward the newer ones, which reach deeper.
 *   Mutations - stacks of bit flips, byte inserts, deletes and replacements; copies
 *               within an input and splices with another; tokens from -dict=FILE
 *               (libFuzzer's format); the values the code compared the input
 *               against, from the trace_cmp and string hooks; and whole lines, since
 *               a config file is lines.
 *   Failures  - ASan and UBSan abort at the first error; the input is written to
 *               <artifact_prefix>crash-<hash> from the sanitizer's death callback, or
 *               from a SIGABRT handler for a harness's own check. A hang becomes
 *               timeout-<hash>, and a leak - looked for after any input that
 *               allocated more than it freed - leak-<hash>.
 *
 * Its command line is libFuzzer's, so CI can run either:
 *   fuzz_<target> [-max_total_time=S] [-runs=N] [-max_len=N] [-dict=FILE] [-seed=N]
 *                 [-timeout=S] [-artifact_prefix=P] [-detect_leaks=0|1] [-merge=1]
 *                 [-edges_only=1] CORPUS_DIR [SEED_DIR ...] | FILE ...
 *   - without -runs or -max_total_time it fuzzes until stopped;
 *   - -runs=0 runs every input once and prints the coverage they reach;
 *   - -merge=1 copies into CORPUS_DIR the smallest-first set of the other
 *     directories' inputs that reaches all of their coverage; with -edges_only=1,
 *     every edge, however often each input hits it (the regression corpus);
 *   - files rather than directories each run once, as reproducers.
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* GCC says __SANITIZE_ADDRESS__; older Clang only answers __has_feature. */
#if defined(__SANITIZE_ADDRESS__)
#  define DRIVER_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define DRIVER_ASAN 1
#  endif
#endif

/* The sanitizer runtime's entry points, when it is linked in: weak, so they are NULL
 * without it. Mach-O cannot link a weak reference to a symbol nothing defines, so
 * there they are named only when ASan is in. */
#if defined(__ELF__) || defined(DRIVER_ASAN)
extern void __sanitizer_set_death_callback(void (*cb)(void)) __attribute__((weak));
extern int  __lsan_do_recoverable_leak_check(void) __attribute__((weak));
extern int  __sanitizer_install_malloc_and_free_hooks(void (*m)(const volatile void *, size_t),
                                                      void (*f)(const volatile void *)) __attribute__((weak));
#  define SAN_DEATH_CB  __sanitizer_set_death_callback
#  define SAN_LEAKCHECK __lsan_do_recoverable_leak_check
#  define SAN_MHOOKS    __sanitizer_install_malloc_and_free_hooks
#else
#  define SAN_DEATH_CB  ((void (*)(void (*)(void)))0)
#  define SAN_LEAKCHECK ((int (*)(void))0)
#  define SAN_MHOOKS    ((int (*)(void (*)(const volatile void *, size_t), void (*)(const volatile void *)))0)
#endif

/* ------------------------------------------------------------------------- */
/* coverage                                                                  */
/* ------------------------------------------------------------------------- */
#define MAP_BITS 17
#define MAP_SIZE (1u << MAP_BITS)

static uint8_t           g_map[MAP_SIZE];    /* this run's hit counters */
static uint8_t           g_seen[MAP_SIZE];   /* the buckets every run so far reached */
static uint8_t           g_bucket[256];
static uint32_t          g_prev;
static volatile int      g_in_target;        /* the hooks record only while the target runs */

/* Where the executable is loaded (GNU ld's symbol, on ELF; elsewhere 0): a block's
 * offset from it is the same in every run, its address is not, so the map - and the
 * coverage a run reports - does not move with ASLR. */
#if defined(__ELF__)
extern char __executable_start __attribute__((weak));
#  define LOAD_BASE ((uintptr_t)&__executable_start)
#else
#  define LOAD_BASE ((uintptr_t)0)
#endif

void __sanitizer_cov_trace_pc(void)
{
    uint64_t pc = (uint64_t)((uintptr_t)__builtin_return_address(0) - LOAD_BASE);
    uint32_t cur = (uint32_t)((pc * 0x9E3779B97F4A7C15ull) >> 40);
    uint32_t i = (cur ^ g_prev) & (MAP_SIZE - 1u);
    if (g_map[i] != 255) g_map[i]++;
    g_prev = cur >> 1;
}

/* With `edges_only`, an edge is one feature however often it is hit: what -merge
 * wants for a regression corpus, which should be small and still reach every edge. */
static void init_buckets(int edges_only)
{
    int c;
    for (c = 0; c < 256; ++c)
        g_bucket[c] = (uint8_t)(c == 0 ? 0 : edges_only || c == 1 ? 1 : c == 2 ? 2 : c == 3 ? 4 : c < 8 ? 8 :
                                c < 16 ? 16 : c < 32 ? 32 : c < 128 ? 64 : 128);
}

static int popcount8(uint8_t v) { int n = 0; while (v) { v &= (uint8_t)(v - 1); ++n; } return n; }

/* The features this run reached that no run did before; merged in when `keep`. */
static size_t new_features(int keep)
{
    size_t i, k, n = 0;
    for (i = 0; i < MAP_SIZE; i += 8) {
        uint64_t w;
        memcpy(&w, g_map + i, 8);
        if (!w) continue;
        for (k = i; k < i + 8; ++k) {
            uint8_t b = g_bucket[g_map[k]], fresh = (uint8_t)(b & ~g_seen[k]);
            if (!fresh) continue;
            n += (size_t)popcount8(fresh);
            if (keep) g_seen[k] |= fresh;
        }
    }
    return n;
}

static void coverage(size_t *edges, size_t *features)
{
    size_t i;
    *edges = *features = 0;
    for (i = 0; i < MAP_SIZE; ++i)
        if (g_seen[i]) { ++*edges; *features += (size_t)popcount8(g_seen[i]); }
}

/* ------------------------------------------------------------------------- */
/* the values the code compared the input against                            */
/* ------------------------------------------------------------------------- */
#define TORC_SIZE 1024
#define TORC_STR  40
typedef struct { uint64_t a, b; uint8_t width; } torc_int;
typedef struct { uint8_t len; uint8_t s[TORC_STR]; } torc_str;
static torc_int g_ti[TORC_SIZE];
static torc_str g_ts[TORC_SIZE];
static unsigned g_tin, g_tsn;   /* filled so far, up to TORC_SIZE */

static void torc_add_int(uint64_t a, uint64_t b, uint8_t width)
{
    unsigned i;
    if (!g_in_target || a == b) return;
    i = (unsigned)((a * 31u + b) % TORC_SIZE);
    g_ti[i].a = a; g_ti[i].b = b; g_ti[i].width = width;
    if (g_tin < TORC_SIZE) ++g_tin;
}

/* No libc string calls in here: the hooks below run inside ASan's interceptors. */
static void torc_add_str(const uint8_t *s, size_t n)
{
    size_t k;
    uint32_t h = 2166136261u;
    unsigned i;
    if (!g_in_target || n == 0) return;
    if (n > TORC_STR) n = TORC_STR;
    for (k = 0; k < n; ++k) { h ^= s[k]; h *= 16777619u; }
    i = h % TORC_SIZE;
    for (k = 0; k < n; ++k) g_ts[i].s[k] = s[k];
    g_ts[i].len = (uint8_t)n;
    if (g_tsn < TORC_SIZE) ++g_tsn;
}

static size_t bounded_strlen(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) ++n;
    return n;
}

void __sanitizer_cov_trace_cmp1(uint8_t a, uint8_t b)        { (void)a; (void)b; }
void __sanitizer_cov_trace_const_cmp1(uint8_t a, uint8_t b)  { (void)a; (void)b; }
void __sanitizer_cov_trace_cmp2(uint16_t a, uint16_t b)       { torc_add_int(a, b, 2); }
void __sanitizer_cov_trace_const_cmp2(uint16_t a, uint16_t b) { torc_add_int(a, b, 2); }
void __sanitizer_cov_trace_cmp4(uint32_t a, uint32_t b)       { torc_add_int(a, b, 4); }
void __sanitizer_cov_trace_const_cmp4(uint32_t a, uint32_t b) { torc_add_int(a, b, 4); }
void __sanitizer_cov_trace_cmp8(uint64_t a, uint64_t b)       { torc_add_int(a, b, 8); }
void __sanitizer_cov_trace_const_cmp8(uint64_t a, uint64_t b) { torc_add_int(a, b, 8); }
void __sanitizer_cov_trace_cmpf(float a, float b)             { (void)a; (void)b; }
void __sanitizer_cov_trace_cmpd(double a, double b)           { (void)a; (void)b; }
void __sanitizer_cov_trace_switch(uint64_t val, uint64_t *cases)
{
    uint64_t i, n = cases[0], width = cases[1] / 8;
    for (i = 0; i < n && i < 16; ++i) torc_add_int(val, cases[2 + i], (uint8_t)(width ? width : 8));
}
void __sanitizer_cov_trace_div4(uint32_t v) { (void)v; }
void __sanitizer_cov_trace_div8(uint64_t v) { (void)v; }
void __sanitizer_cov_trace_gep(uintptr_t i) { (void)i; }
void __sanitizer_cov_trace_pc_indir(uintptr_t c) { (void)c; }

void __sanitizer_weak_hook_strcmp(void *pc, const char *a, const char *b, int r)
{
    (void)pc;
    if (r == 0) return;
    torc_add_str((const uint8_t *)a, bounded_strlen(a, TORC_STR));
    torc_add_str((const uint8_t *)b, bounded_strlen(b, TORC_STR));
}
void __sanitizer_weak_hook_strncmp(void *pc, const char *a, const char *b, size_t n, int r)
{
    (void)pc;
    if (r == 0) return;
    torc_add_str((const uint8_t *)a, bounded_strlen(a, n < TORC_STR ? n : TORC_STR));
    torc_add_str((const uint8_t *)b, bounded_strlen(b, n < TORC_STR ? n : TORC_STR));
}
void __sanitizer_weak_hook_strcasecmp(void *pc, const char *a, const char *b, int r)
{
    __sanitizer_weak_hook_strcmp(pc, a, b, r);
}
void __sanitizer_weak_hook_memcmp(void *pc, const void *a, const void *b, size_t n, int r)
{
    (void)pc;
    if (r == 0) return;
    torc_add_str((const uint8_t *)a, n);
    torc_add_str((const uint8_t *)b, n);
}
void __sanitizer_weak_hook_strstr(void *pc, const char *hay, const char *needle, char *r)
{
    (void)pc; (void)hay;
    if (r) return;
    torc_add_str((const uint8_t *)needle, bounded_strlen(needle, TORC_STR));
}

/* ------------------------------------------------------------------------- */
/* state, options, randomness                                                */
/* ------------------------------------------------------------------------- */
typedef struct { uint8_t *data; size_t len; } unit;

static unit    *g_corpus;
static size_t   g_ncorpus, g_capcorpus;
static unit    *g_dict;
static size_t   g_ndict, g_capdict;

static size_t   g_max_len = 4096;
static long     g_runs_limit = -1;
static long     g_max_time = 0;
static long     g_timeout = 20;
static int      g_detect_leaks = 1;
static int      g_merge = 0;
static uint64_t g_seed = 0;
static char     g_prefix[512] = "./";
static char     g_out_dir[1024];

static uint64_t g_rng;
static unsigned long g_runs;
static const uint8_t *volatile g_cur;        /* the input running now: what a crash saves */
static volatile size_t          g_cur_len;
static volatile uint64_t        g_started;   /* ms when it started, 0 between runs */
static volatile long            g_mallocs, g_frees;

static uint64_t rnd64(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 2685821657736338717ull;
}
static size_t rnd(size_t n) { return n ? (size_t)(rnd64() % n) : 0; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t hash64(const uint8_t *d, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *d++; h *= 1099511628211ull; }
    return h;
}

/* ------------------------------------------------------------------------- */
/* artifacts: written from signal handlers too, so only async-signal-safe calls */
/* ------------------------------------------------------------------------- */
static void put_str(int fd, const char *s) { ssize_t r = write(fd, s, strlen(s)); (void)r; }

static void artifact_name(char *out, size_t cap, const char *dir, const char *kind, const uint8_t *d, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    uint64_t h = hash64(d, n);
    size_t k = 0, i;
    for (i = 0; dir[i] && k + 1 < cap; ++i) out[k++] = dir[i];
    for (i = 0; kind[i] && k + 1 < cap; ++i) out[k++] = kind[i];
    for (i = 0; i < 16 && k + 1 < cap; ++i) out[k++] = hex[(h >> (60 - 4 * i)) & 15u];
    out[k] = 0;
}

static void write_artifact(const char *kind)
{
    char name[700];
    const uint8_t *d = g_cur;
    size_t n = g_cur_len;
    int fd;
    if (!d) return;
    artifact_name(name, sizeof name, g_prefix, kind, d, n);
    fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { ssize_t r = write(fd, d, n); (void)r; close(fd); }
    put_str(2, "\n==driver== the input is saved as ");
    put_str(2, name);
    put_str(2, "\n");
}

static void on_death(void) { if (g_in_target) write_artifact("crash-"); }

static void on_fatal_signal(int sig)
{
    if (g_in_target) write_artifact("crash-");
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_malloc(const volatile void *p, size_t n) { (void)p; (void)n; if (g_in_target) ++g_mallocs; }
static void on_free(const volatile void *p) { if (g_in_target && p) ++g_frees; }

static void *watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        uint64_t s = g_started;
        nanosleep(&ts, NULL);
        if (s && s == g_started && now_ms() - s > (uint64_t)g_timeout * 1000u) {
            put_str(2, "\n==driver== TIMEOUT: one input ran longer than -timeout\n");
            write_artifact("timeout-");
            _exit(70);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* running one input                                                         */
/* ------------------------------------------------------------------------- */
static void run_one(const uint8_t *data, size_t len)
{
    /* a copy of exactly `len` bytes, so a read past the input's end is caught */
    uint8_t *copy = (uint8_t *)malloc(len ? len : 1);
    if (!copy) { fprintf(stderr, "==driver== out of memory\n"); exit(1); }
    if (len) memcpy(copy, data, len);
    memset(g_map, 0, sizeof g_map);
    g_prev = 0;
    g_mallocs = g_frees = 0;
    g_cur = copy; g_cur_len = len;
    g_started = now_ms();
    g_in_target = 1;
    (void)LLVMFuzzerTestOneInput(copy, len);
    g_in_target = 0;
    g_started = 0;
    ++g_runs;
    /* A leak shows as more allocations than frees in the run. The check is costly,
     * so it runs only then; it must run with the input still at hand to save it. */
    if (g_detect_leaks && SAN_LEAKCHECK && g_mallocs > g_frees && SAN_LEAKCHECK()) {
        write_artifact("leak-");
        fprintf(stderr, "==driver== the input above leaked memory\n");
        _exit(71);
    }
    g_cur = NULL; g_cur_len = 0;
    free(copy);
}

static void add_unit(unit **arr, size_t *n, size_t *cap, const uint8_t *d, size_t len)
{
    if (*n == *cap) {
        unit *grown;
        *cap = *cap ? *cap * 2 : 256;
        grown = (unit *)realloc(*arr, *cap * sizeof **arr);
        if (!grown) { fprintf(stderr, "==driver== out of memory\n"); exit(1); }
        *arr = grown;
    }
    (*arr)[*n].data = (uint8_t *)malloc(len ? len : 1);
    if (!(*arr)[*n].data) { fprintf(stderr, "==driver== out of memory\n"); exit(1); }
    if (len) memcpy((*arr)[*n].data, d, len);
    (*arr)[*n].len = len;
    ++*n;
}

static void save_unit(const char *dir, const uint8_t *d, size_t len)
{
    char name[1200];
    FILE *f;
    if (!dir[0]) return;
    snprintf(name, sizeof name, "%s/", dir);
    artifact_name(name + strlen(name), sizeof name - strlen(name), "", "", d, len);
    f = fopen(name, "wb");
    if (!f) { fprintf(stderr, "==driver== cannot write %s: %s\n", name, strerror(errno)); return; }
    if (len) fwrite(d, 1, len, f);
    fclose(f);
}

/* ------------------------------------------------------------------------- */
/* reading inputs and the dictionary                                         */
/* ------------------------------------------------------------------------- */
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0, got;
    if (!f) return NULL;
    for (;;) {
        if (n == cap) {
            uint8_t *grown;
            cap = cap ? cap * 2 : 4096;
            grown = (uint8_t *)realloc(buf, cap);
            if (!grown) { free(buf); fclose(f); return NULL; }
            buf = grown;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    fclose(f);
    *len = n;
    return buf;
}

typedef struct { char *path; size_t len; } file_ent;

static int cmp_files(const void *a, const void *b)
{
    const file_ent *x = (const file_ent *)a, *y = (const file_ent *)b;
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return strcmp(x->path, y->path);
}

/* Every regular file in `dir`, appended to *out. */
static void list_dir(const char *dir, file_ent **out, size_t *n, size_t *cap)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        char p[1200];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (*n == *cap) {
            file_ent *grown;
            *cap = *cap ? *cap * 2 : 256;
            grown = (file_ent *)realloc(*out, *cap * sizeof **out);
            if (!grown) { closedir(d); return; }
            *out = grown;
        }
        (*out)[*n].path = (char *)malloc(strlen(p) + 1);
        if (!(*out)[*n].path) break;
        strcpy((*out)[*n].path, p);
        (*out)[*n].len = (size_t)st.st_size;
        ++*n;
    }
    closedir(d);
}

static int hexdig(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* libFuzzer's (and AFL's) dictionary format: one "token" per line, optionally
 * name="token", with \\ \" and \xNN escapes; # starts a comment. */
static void load_dict(const char *path)
{
    size_t len, i = 0;
    uint8_t *text = read_file(path, &len);
    if (!text) { fprintf(stderr, "==driver== cannot read the dictionary %s\n", path); exit(1); }
    while (i < len) {
        size_t end = i, k;
        uint8_t tok[256];
        size_t tn = 0;
        while (end < len && text[end] != '\n') ++end;
        for (k = i; k < end && text[k] != '"' && text[k] != '#'; ++k) { }
        if (k < end && text[k] == '"') {
            for (++k; k < end && text[k] != '"' && tn < sizeof tok; ++k) {
                if (text[k] == '\\' && k + 1 < end) {
                    ++k;
                    if (text[k] == 'x' && k + 2 < end && hexdig(text[k + 1]) >= 0 && hexdig(text[k + 2]) >= 0) {
                        tok[tn++] = (uint8_t)(hexdig(text[k + 1]) * 16 + hexdig(text[k + 2]));
                        k += 2;
                    } else {
                        tok[tn++] = text[k];
                    }
                } else {
                    tok[tn++] = text[k];
                }
            }
            if (tn) add_unit(&g_dict, &g_ndict, &g_capdict, tok, tn);
        }
        i = end + 1;
    }
    free(text);
    fprintf(stderr, "==driver== %lu dictionary tokens from %s\n", (unsigned long)g_ndict, path);
}

/* ------------------------------------------------------------------------- */
/* mutations                                                                 */
/* ------------------------------------------------------------------------- */
static const uint8_t  INT8[]  = { 0, 1, 2, 7, 8, 15, 16, 31, 32, 63, 64, 100, 127, 128, 129, 200, 255 };
static const uint16_t INT16[] = { 0, 1, 255, 256, 4095, 4096, 4097, 32767, 32768, 65535 };
static const uint32_t INT32[] = { 0, 1, 255, 65535, 65536, 0x0fffffffu, 0x10000000u, 0x10000001u,
                                  0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu };

static size_t insert_bytes(uint8_t *b, size_t n, size_t max, size_t at, const uint8_t *src, size_t k)
{
    if (k == 0 || n + k > max) return n;
    if (at > n) at = n;
    memmove(b + at + k, b + at, n - at);
    memcpy(b + at, src, k);
    return n + k;
}

static size_t overwrite_bytes(uint8_t *b, size_t n, size_t max, size_t at, const uint8_t *src, size_t k)
{
    if (at > n) at = n;
    if (at + k > max) { if (at >= max) return n; k = max - at; }
    memcpy(b + at, src, k);
    return at + k > n ? at + k : n;
}

/* The bounds of a random line: [*s, *e), without its '\n'. 0 if there is none. */
static int pick_line(const uint8_t *b, size_t n, size_t *s, size_t *e)
{
    size_t at;
    if (n == 0) return 0;
    at = rnd(n);
    *s = at; while (*s > 0 && b[*s - 1] != '\n') --*s;
    *e = at; while (*e < n && b[*e] != '\n') ++*e;
    return 1;
}

static void put_le(uint8_t *out, uint64_t v, size_t w) { size_t i; for (i = 0; i < w; ++i) out[i] = (uint8_t)(v >> (8 * i)); }
static void put_be(uint8_t *out, uint64_t v, size_t w) { size_t i; for (i = 0; i < w; ++i) out[i] = (uint8_t)(v >> (8 * (w - 1 - i))); }

static size_t mutate(uint8_t *b, size_t n, size_t max)
{
    size_t at = n ? rnd(n) : 0, k;
    uint8_t tmp[64];
    switch (rnd(16)) {
    case 0:                                         /* flip a bit */
        if (n) b[at] ^= (uint8_t)(1u << rnd(8));
        return n;
    case 1:                                         /* a random byte */
        if (n) b[at] = (uint8_t)rnd(256);
        return n;
    case 2:                                         /* an interesting byte */
        if (n) b[at] = INT8[rnd(sizeof INT8)];
        return n;
    case 3:                                         /* insert bytes: random, or one repeated */
        k = 1 + rnd(4);
        if (rnd(2)) { size_t i; for (i = 0; i < k; ++i) tmp[i] = (uint8_t)rnd(256); }
        else memset(tmp, (int)(n ? b[rnd(n)] : rnd(256)), k);
        return insert_bytes(b, n, max, rnd(n + 1), tmp, k);
    case 4:                                         /* delete a range */
        if (n == 0) return n;
        k = 1 + rnd(n - at < 16 ? n - at : 16);
        memmove(b + at, b + at + k, n - at - k);
        return n - k;
    case 5: case 6:                                 /* a dictionary token, inserted or written over */
        if (!g_ndict) return n;
        { const unit *u = &g_dict[rnd(g_ndict)];
          return rnd(2) ? insert_bytes(b, n, max, rnd(n + 1), u->data, u->len)
                        : overwrite_bytes(b, n, max, at, u->data, u->len); }
    case 7:                                         /* a string the code compared against */
        if (!g_tsn) return n;
        { const torc_str *s = &g_ts[rnd(TORC_SIZE)];
          if (!s->len) return n;
          return rnd(2) ? insert_bytes(b, n, max, rnd(n + 1), s->s, s->len)
                        : overwrite_bytes(b, n, max, at, s->s, s->len); }
    case 8: {                                       /* an integer the code compared against */
        const torc_int *t;
        size_t w, i;
        uint8_t from[8], to[8];
        if (!g_tin) return n;
        t = &g_ti[rnd(TORC_SIZE)];
        if (!t->width) return n;
        w = t->width;
        if (rnd(2)) { put_le(from, t->a, w); put_le(to, t->b + (uint64_t)((int)rnd(3) - 1), w); }
        else        { put_be(from, t->a, w); put_be(to, t->b + (uint64_t)((int)rnd(3) - 1), w); }
        for (i = 0; n >= w && i + w <= n; ++i) {    /* where the input holds one side, the other */
            size_t j = (at + i) % (n - w + 1);
            if (!memcmp(b + j, from, w)) { memcpy(b + j, to, w); return n; }
        }
        return rnd(2) ? insert_bytes(b, n, max, rnd(n + 1), to, w) : overwrite_bytes(b, n, max, at, to, w);
    }
    case 9: {                                       /* copy a chunk within the input */
        size_t from, len;
        if (n < 2) return n;
        from = rnd(n); len = 1 + rnd(n - from < 64 ? n - from : 64);
        memcpy(tmp, b + from, len);
        return rnd(2) ? insert_bytes(b, n, max, rnd(n + 1), tmp, len) : overwrite_bytes(b, n, max, at, tmp, len);
    }
    case 10: case 11: {                             /* splice with another input */
        const unit *u;
        size_t from, len;
        if (!g_ncorpus) return n;
        u = &g_corpus[rnd(g_ncorpus)];
        if (!u->len) return n;
        from = rnd(u->len); len = 1 + rnd(u->len - from);
        if (rnd(2)) {                               /* crossover: this input's head, its tail */
            size_t cut = rnd(n + 1);
            if (cut + len > max) len = max - cut;
            memcpy(b + cut, u->data + from, len);
            return cut + len;
        }
        if (len > max - n) len = max - n;
        return insert_bytes(b, n, max, rnd(n + 1), u->data + from, len);
    }
    case 12: {                                      /* arithmetic on a little-endian integer */
        size_t w = (size_t)1 << rnd(3);
        uint64_t v = 0;
        size_t i;
        if (n < w) return n;
        at = rnd(n - w + 1);
        for (i = 0; i < w; ++i) v |= (uint64_t)b[at + i] << (8 * i);
        v += (uint64_t)((int64_t)rnd(71) - 35);
        put_le(b + at, v, w);
        return n;
    }
    case 13: {                                      /* an interesting 16- or 32-bit value */
        if (rnd(2) && n >= 2) { put_le(b + rnd(n - 1), INT16[rnd(sizeof INT16 / sizeof INT16[0])], 2); return n; }
        if (n >= 4) put_le(b + rnd(n - 3), INT32[rnd(sizeof INT32 / sizeof INT32[0])], 4);
        return n;
    }
    case 14: {                                      /* a whole line: doubled, or dropped */
        size_t s, e, len;
        if (!pick_line(b, n, &s, &e)) return n;
        len = e - s + (e < n ? 1 : 0);
        if (rnd(2)) {
            if (len == 0 || n + len > max) return n;
            memmove(b + s + len, b + s, n - s);     /* the line, then a copy of it */
            return n + len;
        }
        memmove(b + s, b + s + len, n - s - len);
        return n - len;
    }
    default: {                                      /* swap a line with the one after it */
        size_t s, e, s2, e2, l1, l2;
        uint8_t *t;
        if (!pick_line(b, n, &s, &e) || e >= n) return n;
        s2 = e + 1; e2 = s2;
        while (e2 < n && b[e2] != '\n') ++e2;
        l1 = e - s; l2 = e2 - s2;
        t = (uint8_t *)malloc(l1 + l2 + 1);
        if (!t) return n;
        memcpy(t, b + s2, l2);
        t[l2] = '\n';
        memcpy(t + l2 + 1, b + s, l1);
        memcpy(b + s, t, l1 + l2 + 1);
        free(t);
        return n;
    }
    }
}

/* ------------------------------------------------------------------------- */
/* the loop                                                                  */
/* ------------------------------------------------------------------------- */
static uint64_t g_t0;

static void stats(const char *what)
{
    size_t edges, feats, bytes = 0, i;
    uint64_t el = now_ms() - g_t0;
    coverage(&edges, &feats);
    for (i = 0; i < g_ncorpus; ++i) bytes += g_corpus[i].len;
    fprintf(stderr, "#%lu\t%s cov: %lu ft: %lu corp: %lu/%lub exec/s: %lu time: %lus\n",
            g_runs, what, (unsigned long)edges, (unsigned long)feats, (unsigned long)g_ncorpus,
            (unsigned long)bytes, (unsigned long)(el ? g_runs * 1000u / el : 0), (unsigned long)(el / 1000u));
}

/* Run an input; keep it if it reached something new. Returns the new features. */
static size_t try_unit(const uint8_t *d, size_t len, int save)
{
    size_t nf;
    run_one(d, len);
    nf = new_features(1);
    if (nf) {
        add_unit(&g_corpus, &g_ncorpus, &g_capcorpus, d, len);
        if (save) save_unit(g_out_dir, d, len);
    }
    return nf;
}

static void usage(void)
{
    fprintf(stderr, "usage: fuzz_<target> [-max_total_time=S] [-runs=N] [-max_len=N] [-dict=FILE] [-seed=N]\n"
                    "       [-timeout=S] [-artifact_prefix=P] [-detect_leaks=0|1] [-merge=1] [-edges_only=1]\n"
                    "       CORPUS_DIR [SEED_DIR ...] | FILE ...\n");
    exit(2);
}

static char **dirs, **files;   /* the command line's directories and files */

/* `path` as a directory, and every directory above it that is missing: the corpus
 * directory of a first run, work/toml say, where there is no work/ yet. 0, or -1 with
 * errno. */
static int make_dirs(const char *path)
{
    char p[1024];
    size_t i, n = strlen(path);
    if (n == 0 || n >= sizeof p) { errno = ENAMETOOLONG; return -1; }
    memcpy(p, path, n + 1);
    for (i = 1; i <= n; ++i) {
        if (p[i] != '/' && p[i] != 0) continue;
        if (p[i - 1] == '/') continue;                 /* a doubled or trailing slash */
        p[i] = 0;
        if (mkdir(p, 0755) != 0 && errno != EEXIST) return -1;
        p[i] = path[i];
    }
    return 0;
}

int main(int argc, char **argv)
{
    size_t ndirs = 0, nfiles = 0, i;
    file_ent *ents = NULL;
    size_t nents = 0, capents = 0;
    uint8_t *buf;
    uint64_t last_print;
    pthread_t wd;
    int edges_only = 0;

    dirs = (char **)calloc((size_t)argc, sizeof *dirs);
    files = (char **)calloc((size_t)argc, sizeof *files);
    if (!dirs || !files) return 1;
    for (i = 1; i < (size_t)argc; ++i) {
        const char *a = argv[i];
        struct stat st;
        if (a[0] == '-') {
            if      (!strncmp(a, "-max_total_time=", 16)) g_max_time = atol(a + 16);
            else if (!strncmp(a, "-runs=", 6))            g_runs_limit = atol(a + 6);
            else if (!strncmp(a, "-max_len=", 9))         g_max_len = (size_t)atol(a + 9);
            else if (!strncmp(a, "-dict=", 6))            load_dict(a + 6);
            else if (!strncmp(a, "-seed=", 6))            g_seed = (uint64_t)strtoull(a + 6, NULL, 10);
            else if (!strncmp(a, "-timeout=", 9))         g_timeout = atol(a + 9);
            else if (!strncmp(a, "-artifact_prefix=", 17)) snprintf(g_prefix, sizeof g_prefix, "%s", a + 17);
            else if (!strncmp(a, "-detect_leaks=", 14))   g_detect_leaks = atoi(a + 14);
            else if (!strncmp(a, "-merge=", 7))           g_merge = atoi(a + 7);
            else if (!strncmp(a, "-edges_only=", 12))     edges_only = atoi(a + 12);
            else if (!strcmp(a, "-help") || !strcmp(a, "-h")) usage();
            else fprintf(stderr, "==driver== ignoring %s\n", a);   /* libFuzzer flags this driver lacks */
            continue;
        }
        if (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) dirs[ndirs++] = argv[i];
        else if (stat(a, &st) == 0 && S_ISREG(st.st_mode)) files[nfiles++] = argv[i];
        else if (ndirs == 0 && nfiles == 0) {        /* the corpus directory, made if it is not there */
            if (make_dirs(a) != 0) { fprintf(stderr, "==driver== cannot make %s: %s\n", a, strerror(errno)); return 1; }
            dirs[ndirs++] = argv[i];
        } else {
            fprintf(stderr, "==driver== no such file or directory: %s\n", a);
            return 1;
        }
    }
    if (g_max_len == 0) g_max_len = 4096;
    init_buckets(edges_only);
    g_rng = g_seed ? g_seed : (now_ms() ^ ((uint64_t)getpid() << 32)) | 1u;
    if (!g_seed) g_seed = g_rng;
    g_rng |= 1u;
    if (SAN_DEATH_CB) SAN_DEATH_CB(on_death);
    if (SAN_MHOOKS) SAN_MHOOKS(on_malloc, on_free);
    else g_detect_leaks = 0;
    signal(SIGABRT, on_fatal_signal);
#if !defined(DRIVER_ASAN)
    signal(SIGSEGV, on_fatal_signal);   /* ASan has its own, which calls on_death */
    signal(SIGBUS, on_fatal_signal);
    signal(SIGFPE, on_fatal_signal);
    signal(SIGILL, on_fatal_signal);
#endif
    if (pthread_create(&wd, NULL, watchdog, NULL) == 0) pthread_detach(wd);
    g_t0 = now_ms();

    /* reproducers: each file once */
    if (nfiles && !ndirs) {
        for (i = 0; i < nfiles; ++i) {
            size_t len;
            uint8_t *d = read_file(files[i], &len);
            if (!d) { fprintf(stderr, "==driver== cannot read %s\n", files[i]); return 1; }
            fprintf(stderr, "==driver== running %s (%lu bytes)\n", files[i], (unsigned long)len);
            run_one(d, len);
            free(d);
        }
        fprintf(stderr, "==driver== %lu input(s) ran without a failure\n", (unsigned long)nfiles);
        return 0;
    }
    if (!ndirs) usage();
    snprintf(g_out_dir, sizeof g_out_dir, "%s", dirs[0]);

    /* every input, smallest first: a small one that reaches the same is the one kept */
    for (i = (g_merge ? 1u : 0u); i < ndirs; ++i) list_dir(dirs[i], &ents, &nents, &capents);
    if (nents) qsort(ents, nents, sizeof *ents, cmp_files);
    if (g_merge) {                                   /* what the output already reaches counts first */
        file_ent *out = NULL;
        size_t nout = 0, capout = 0;
        list_dir(dirs[0], &out, &nout, &capout);
        for (i = 0; i < nout; ++i) {
            size_t len;
            uint8_t *d = read_file(out[i].path, &len);
            if (d) { if (len > g_max_len) len = g_max_len; (void)try_unit(d, len, 0); free(d); }
            free(out[i].path);
        }
        free(out);
    }
    for (i = 0; i < nents; ++i) {
        size_t len;
        uint8_t *d = read_file(ents[i].path, &len);
        if (!d) continue;
        if (len > g_max_len) len = g_max_len;
        (void)try_unit(d, len, g_merge);
        free(d);
        free(ents[i].path);
    }
    free(ents);
    if (g_ncorpus == 0) { static const uint8_t nl = '\n'; (void)try_unit(&nl, 1, 0); }
    stats(g_merge ? "MERGED" : "INITED");
    if (g_merge || g_runs_limit == 0) { stats("DONE"); return 0; }

    buf = (uint8_t *)malloc(g_max_len + 1);
    if (!buf) return 1;
    fprintf(stderr, "==driver== seed %llu, max_len %lu\n", (unsigned long long)g_seed, (unsigned long)g_max_len);
    last_print = now_ms();
    for (;;) {
        const unit *base;
        size_t len, depth, k, nf;
        uint64_t t = now_ms();
        if (g_runs_limit > 0 && g_runs >= (unsigned long)g_runs_limit) break;
        if (g_max_time > 0 && t - g_t0 >= (uint64_t)g_max_time * 1000u) break;
        if (t - last_print >= 5000) { stats("pulse"); last_print = t; }
        /* newer entries reached deeper: lean toward them */
        k = rnd(g_ncorpus); i = rnd(g_ncorpus);
        base = &g_corpus[k > i ? k : i];
        len = base->len > g_max_len ? g_max_len : base->len;
        memcpy(buf, base->data, len);
        depth = (size_t)1 << rnd(4);
        for (k = 0; k < depth; ++k) len = mutate(buf, len, g_max_len);
        nf = try_unit(buf, len, 1);
        if (nf) stats("NEW");
    }
    stats("DONE");
    free(buf);
    return 0;
}
