/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_durable_crash.c - the durable queue, killed at each of its I/O calls in turn.
 *
 * gptps_durable_queue.h makes promises about crashes: a submit that returned is
 * recovered, a retraction that returned stays retracted, a torn tail is dropped
 * silently, a compaction leaves the old journal or the new one. test_durable.c kills
 * a process at a few chosen moments. This one kills it at EVERY I/O call the queue
 * makes during a workload, in several ways, and checks the promises after each death.
 *
 * A child process runs the workload (child_main): it opens a journal a previous run
 * left behind - none, one with survivors of crashes, one damaged, one cut short (see
 * "rounds") - recovers it, submits singly and in a batch, runs work that finishes,
 * fails and cancels itself, retracts, compacts from outside and from inside a task, and
 * drains the quarantine. It ends on one thread, or with three threads submitting at
 * once, so that they share fsyncs while the main thread retracts and compacts. It dies
 * at its Nth I/O call on the journal's files, and N runs over every call the workload
 * makes. At that call:
 *   before      - SIGKILL before making it;
 *   after       - SIGKILL after it;
 *   torn        - an fwrite puts a random prefix of its bytes in the file, then SIGKILL;
 *   power       - SIGKILL after it, and the power fails: each file goes back to what
 *                 its last fsync made durable, and each name to what the directory's
 *                 last fsync made durable;
 *   error       - the call fails (EIO, a full disk), the workload carries on to its
 *                 end and dies there;
 *   error+power - the same, and the power fails at the end;
 *   error+power at the next fsync - the same, but the power fails just before the
 *                 next fsync, while nothing since the failure is durable yet;
 *   two errors+power - the call fails, and so does the next fsync after it (the
 *                 directory's next one, if the call was the directory's): the second
 *                 failure lands where the queue undoes the first, which can break the
 *                 queue (see "a broken queue" in the add-on). The workload goes on, and
 *                 the power fails at the end.
 * The parent then recovers the journal, twice, and checks (judge, verify):
 *   - gptps_dq_open succeeds, without crashing or hanging - and where the child's open
 *     returned NULL, it had left the journal as it was;
 *   - every record whose submit returned GPTPS_OK is there, with its own name and
 *     payload, unless it was retracted, or closed and compacted away since;
 *   - nothing is there that was never submitted, whose submit failed, or whose
 *     retraction returned GPTPS_OK, and nothing is there twice - except that a submit
 *     or a retraction that failed as the queue broke may still have reached the
 *     journal, until a compaction repaired the queue;
 *   - a dead-lettered record is kept until a drain has compacted it away;
 *   - after a process crash, each record's crash count is what its attempts say;
 *   - the journal itself is never gone, whatever a compaction was doing (but a new one
 *     whose name no sync of the directory made durable before the power failed);
 *   - a torn tail is dropped without a warning, and a journal cut short by damage is
 *     compacted only once a copy of it has been kept (one whose damage is confined,
 *     also when the copy failed);
 *   - the second recovery loads exactly what the first one did;
 *   - and then everything recovered runs, once.
 * Then the workload runs once more for each journal, uncrashed, with every fsync of
 * the directory failing with EINVAL, as on a file system that cannot sync one: it must
 * go exactly as written. And a few cases run on their own (see "single cases"): the
 * ".corrupt" copies when all ten exist or one cannot be read, a damaged journal no copy
 * of which can be made, and a directory this process may write but not read.
 *
 * How. The add-on is compiled into this file, so the parent can read what a recovery
 * loaded straight from its record table (as test_durable_oom.c does), and the link
 * wraps the calls it makes on files (-Wl,--wrap=fopen,... - see CMakeLists.txt). A
 * call on one of the journal's files is counted and, if it is the chosen one, acted
 * on; every other call passes straight through. The fsync wrapper is a model of one:
 * it notes what a real fsync would make durable, in memory the parent can read after
 * the child is gone, and skips the disk - so a sweep of thousands of runs never waits
 * for one, whatever the file system, and "power" can be simulated exactly.
 *
 * Linux only: --wrap is a GNU ld option, and the model reads a file back through
 * /proc/self/fd. `test_durable_crash 20` sweeps twenty seeds (torn and short-write
 * lengths) instead of one; CI runs one. GPTPS_CRASH_SYNC_DELAY_US=3000 makes every
 * fsync of the journal wait that long before the model reads the file, which widens
 * the window in which the other submitters write while one of them syncs.
 */
#undef _FORTIFY_SOURCE          /* fortified, fread becomes __fread_chk, which --wrap=fread misses */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../addons/gptps_durable_queue.c"

/* The real calls, and the wrappers the linker sends the add-on's calls to. */
FILE  *__real_fopen(const char *path, const char *mode);
size_t __real_fread(void *p, size_t size, size_t n, FILE *f);
size_t __real_fwrite(const void *p, size_t size, size_t n, FILE *f);
int    __real_fflush(FILE *f);
int    __real_fclose(FILE *f);
int    __real_fsync(int fd);
int    __real_ftruncate(int fd, off_t len);
int    __real_rename(const char *from, const char *to);
int    __real_remove(const char *path);
FILE  *__wrap_fopen(const char *path, const char *mode);
size_t __wrap_fread(void *p, size_t size, size_t n, FILE *f);
size_t __wrap_fwrite(const void *p, size_t size, size_t n, FILE *f);
int    __wrap_fflush(FILE *f);
int    __wrap_fclose(FILE *f);
int    __wrap_fsync(int fd);
int    __wrap_ftruncate(int fd, off_t len);
int    __wrap_rename(const char *from, const char *to);
int    __wrap_remove(const char *path);

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define JOURNAL  "dq_crash.journal"
#define NNAME    12             /* the journal, ".tmp", ".corrupt", ".corrupt.1" .. ".9" */
#define NID      64             /* record ids */
#define NEV      2048           /* I/O calls one run may make */
#define NCOMP    8              /* compactions one run may make */
#define NFILE    24             /* files (inodes) the power model follows in one run */
#define IMG_MAX  (64 * 1024)    /* the largest file the power model keeps */
#define NGOT     128            /* records one recovery may load */
#define PL_MAX   6000           /* the largest payload: BIG_ID's */
#define BIG_ID   25             /* one record bigger than stdio's buffer */
#ifndef SHOW_MAX
#  define SHOW_MAX 40           /* failures printed in full; the rest are counted */
#endif
/* Under ASan or TSan a run costs fifteen times as much - the fork and the child's first
 * writes copy shadow memory - so a sanitized build tries one call in SWEEP_STRIDE, a
 * different one for each seed and round, to stay inside its CI timeout. They are there
 * for the memory and the races; every call is swept by the plain builds. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define SWEEP_STRIDE 4
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define SWEEP_STRIDE 4
#  endif
#endif
#ifndef SWEEP_STRIDE
#  define SWEEP_STRIDE 1
#endif
#define STR_(x) #x
#define STR(x)  STR_(x)

#define EXIT_END     102        /* the child reached the end of the workload */
#define EXIT_BAD     103        /* it saw something it should not have: S->bad says what */

enum { M_BEFORE, M_AFTER, M_TORN, M_POWER, M_ERROR, M_ERRPOWER, M_ERRSYNC, M_ERR2, M_N };
static const char *const mode_name[M_N] = {
    "before", "after", "torn", "power", "error", "error+power", "error+power at the next fsync",
    "two errors+power" };

enum { IO_OPEN_R, IO_OPEN_W, IO_READ, IO_WRITE, IO_FLUSH, IO_CLOSE, IO_SYNC, IO_DIRSYNC,
       IO_TRUNC, IO_RENAME, IO_REMOVE, IO_N };
static const char *const io_name[IO_N] = {
    "fopen(r)", "fopen(w)", "fread", "fwrite", "fflush", "fclose", "fsync", "fsync",
    "ftruncate", "rename", "remove" };

/* What the child did to one record, as ticks of one clock (0: not done). The child
 * writes these and the parent reads them once the child is dead: shared memory keeps
 * whatever was written before the kill, as a pipe would, and needs no reader meanwhile. */
typedef struct {
    uint32_t sub_call, sub_ret;   /* gptps_dq_submit or _batch was called, returned */
    int32_t  sub_st;
    uint32_t sub_broken;          /* it failed, and the queue was broken as it returned */
    uint32_t can_call, can_ret;   /* gptps_dq_cancel */
    int32_t  can_st;
    uint32_t can_broken;
    uint32_t closed;              /* the queue closed it: finished, dropped, cancelled itself */
    uint32_t quar;                /* dead-lettered */
    uint32_t drained;             /* the drain's callback saw it */
    uint32_t start, end;          /* its latest attempt began, ended - as the queue journaled them */
    uint32_t busy, busy_close;    /* the queue's observer is journaling one of its events, a closing one */
    uint32_t k_open;              /* its crash count once the child's gptps_dq_open returned, + 1 */
} fact;

typedef struct { uint32_t call, ret; int32_t st; } comp_fact;

/* A file as the power model sees it: what its last fsync made durable. Two images,
 * so that a crash in the middle of copying one leaves the previous one standing. */
typedef struct {
    uint64_t      ino;
    uint32_t      pub;            /* img[pub - 1] is durable; 0: nothing ever was */
    uint32_t      len[2];
    unsigned char img[2][IMG_MAX];
} file_model;

typedef struct {
    /* the plan, set by the parent before each fork */
    int32_t    mode;
    uint32_t   at;                /* the I/O call to act at, from 1; 0: none */
    uint32_t   seed;
    int32_t    round;
    int32_t    group;             /* three threads submit at the end: see child_main */
    int32_t    dir_einval;        /* every fsync of the directory fails with EINVAL */
    /* the child's account */
    uint32_t   events, clock;
    uint32_t   hit;               /* the call acted at, once it was made */
    int32_t    hit_io, hit_name;
    uint32_t   hit_len, hit_cut;  /* a torn or short write: bytes asked for, bytes written */
    uint32_t   hit2;              /* two errors: the call the second one was made at */
    int32_t    hit2_io;
    uint32_t   broke;             /* the queue was seen broken */
    uint8_t    ev_io[NEV];        /* each call's kind, in the order they were made */
    uint32_t   io_seen[IO_N];
    uint32_t   open_ret;
    int32_t    open_ok;
    uint32_t   open_events;       /* the I/O calls made when gptps_dq_open returned */
    uint32_t   drain_call, drain_ret, drain_n;
    int32_t    drain_st;
    uint32_t   ncomp;
    comp_fact  comp[NCOMP];
    fact       id[NID];
    char       bad[200];
    /* the power model: what the directory's last fsync made durable */
    uint32_t   dir_pub;
    int32_t    dir[2][NNAME];     /* name -> file index + 1; 0: no such file */
    uint32_t   nfile;             /* everything below is reset by the child as it is used */
    file_model file[NFILE];
} shared;

static shared   *S;
static int       g_child;         /* this process is the child: the wrappers act */
static long      g_sync_delay_us; /* GPTPS_CRASH_SYNC_DELAY_US */
static int       g_pfail_name = -1, g_pfail_skip;   /* see __wrap_fopen */
static char      g_names[NNAME][40];
static dev_t     g_cwd_dev;
static ino_t     g_cwd_ino;

static uint32_t tick(void)                    { return __atomic_add_fetch(&S->clock, 1, __ATOMIC_SEQ_CST); }
static void     st32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static void     sti32(int32_t *p, int32_t v)  { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static uint32_t ld32(const uint32_t *p)       { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static int      failing(void)                 { return S->mode >= M_ERROR; }
static int      powered(int mode)             { return mode == M_POWER || mode >= M_ERRPOWER; }

static void child_bad(const char *what)
{
    snprintf(S->bad, sizeof S->bad, "%s", what);
    _exit(EXIT_BAD);
}

static int name_index(const char *path)       /* -1: not one of the journal's files */
{
    int i;
    if (!path || strncmp(path, JOURNAL, sizeof JOURNAL - 1) != 0) return -1;
    for (i = 0; i < NNAME; ++i) if (strcmp(path, g_names[i]) == 0) return i;
    return -1;
}

/* ---- records: ids, kinds, payloads ----
 * Every record the workload writes carries its id, and the rest of its payload is a
 * function of the id, so the parent knows each one it loads - and knows one that
 * nobody wrote, or that came back with the wrong name or bytes. */
enum { K_OK, K_FAIL, K_SELF, K_COMPACTOR, K_NOSUCH, K_N };
static const char *const kind_name[K_N] = { "ok", "fail", "self", "compactor", "nosuch" };
static int kind_of(int id)
{
    switch (id) {
    case 5: case 21: return K_FAIL;      /* dead-lettered: quarantined */
    case 9: case 22: return K_SELF;      /* its body cancels itself: closed */
    case 29:         return K_COMPACTOR; /* compacts the journal while it runs */
    case 27:         return K_NOSUCH;    /* never registered: the engine refuses it */
    default:         return K_OK;
    }
}
static size_t make_payload(int id, unsigned char *out)   /* out holds PL_MAX bytes */
{
    size_t n = id == BIG_ID ? PL_MAX : 8 + (size_t)(id * 7 % 23), j;
    put32(out, (uint32_t)id);
    put32(out + 4, fnv(&id, sizeof id, DQ_FNV_SEED));
    for (j = 8; j < n; ++j) out[j] = (unsigned char)(id + (int)j);
    return n;
}
static int id_of(const void *p, size_t len)   /* the id a payload carries; -1: none we wrote */
{
    unsigned char want[PL_MAX];
    int id;
    if (!p || len < 8) return -1;
    id = (int)get32((const unsigned char *)p);
    if (id <= 0 || id >= NID) return -1;
    if (make_payload(id, want) != len || memcmp(want, p, len) != 0) return -1;
    return id;
}

/* ---- what the parent leaves behind for the child: rounds ----
 * fresh     - no journal yet: the child's gptps_dq_open creates it.
 * survivors - a journal a crashed run left: pending records, one that was running at
 *             one death (2), two suspects at two (3, 4), one dead-lettered (5), one
 *             closed (6), one running at a third death (7: quarantined at open), one
 *             whose attempt ended (8), and a record torn at the end (10).
 * damaged   - the same with record 2's payload damaged: gptps_dq_open skips it, keeps
 *             the rest, and copies the journal aside before it compacts it.
 * cut       - the same with record 4's length pushed past the end of the file, which
 *             looks like a torn write: the records after it are "reported and kept in
 *             the copy, but not loaded" (the header's words).
 * What each id is at the start, and the crash count a recovery gives it: */
enum { R_FRESH, R_SURVIVORS, R_DAMAGED, R_CUT, R_N };
static const char *const round_name[R_N] = { "fresh", "survivors", "damaged", "cut" };
enum { I_NONE, I_PENDING, I_QUAR, I_DONE, I_LOST, I_CUT };
static int init_state(int round, int id)
{
    if (round == R_FRESH || id < 1 || id > 10) return I_NONE;
    if (round == R_CUT && id >= 4) return id == 4 || id == 10 ? I_LOST : I_CUT;
    switch (id) {
    case 2:  return round == R_DAMAGED ? I_LOST : I_PENDING;
    case 5:  case 7: return I_QUAR;
    case 6:  return I_DONE;
    case 10: return I_LOST;
    default: return I_PENDING;
    }
}
static uint32_t init_crashes(int round, int id)
{
    if (init_state(round, id) == I_NONE) return 0;
    switch (id) { case 2: return 1; case 3: case 4: return 2; case 7: return 3; default: return 0; }
}
static unsigned recovers_at_start(int round)   /* gptps_dq_recover's count: the pending, and suspect 3 */
{
    static const unsigned n[R_N] = { 0, 5, 4, 3 };   /* -; 1 2 8 9 3; 1 8 9 3; 1 2 3 */
    return n[round];
}
static unsigned drained_at_end(int round)      /* 21, and 5 and 7 where they were loaded */
{
    return round == R_FRESH || round == R_CUT ? 1u : 3u;
}

static long jp(FILE *f, int id)                /* a 'P' for id; returns where it starts */
{
    unsigned char pl[PL_MAX];
    size_t n = make_payload(id, pl);
    long at = ftell(f);
    write_record(f, 'P', (uint64_t)id, kind_name[kind_of(id)], pl, n);
    return at;
}
static void jm(FILE *f, char type, int id) { write_record(f, type, (uint64_t)id, "", NULL, 0); }
static void jk(FILE *f, int id, uint32_t k) { unsigned char b[4]; put32(b, k); write_record(f, 'K', (uint64_t)id, "", b, 4); }

static unsigned char g_orig[IMG_MAX];   /* the journal a round starts from, as written */
static long          g_orig_len;

static void write_journal(const char *path, int round)   /* a round's journal, at `path` */
{
    FILE *f;
    long p2, p4, p10;
    f = fopen(path, "wb");
    CHECK(f != NULL); if (!f) return;
    write_file_header(f);
    jp(f, 1);
    p2 = jp(f, 2); jm(f, 'S', 2);
    jp(f, 3); jk(f, 3, 1); jm(f, 'S', 3);
    p4 = jp(f, 4); jk(f, 4, 1); jm(f, 'S', 4);
    jp(f, 5); jm(f, 'Q', 5);
    jp(f, 6); jm(f, 'D', 6);
    jp(f, 7); jk(f, 7, 2); jm(f, 'S', 7);
    jp(f, 8); jm(f, 'S', 8); jm(f, 'F', 8);
    jp(f, 9);
    p10 = jp(f, 10);
    CHECK(fclose(f) == 0);
    CHECK(truncate(path, (off_t)(p10 + 30)) == 0);          /* 10 is torn: 30 bytes of it */
    if (round == R_DAMAGED || round == R_CUT) {
        /* a bit flipped in 2's payload, or 4's payload length grown by a MiB */
        off_t at = round == R_DAMAGED ? (off_t)(p2 + DQ_RHDR_LEN + 2 + 5) : (off_t)(p4 + 8 + 2);
        int fd = open(path, O_RDWR);
        unsigned char b = 0;
        CHECK(fd >= 0 && pread(fd, &b, 1, at) == 1);
        b ^= 0x10;
        CHECK(fd >= 0 && pwrite(fd, &b, 1, at) == 1);
        if (fd >= 0) close(fd);
    }
    f = fopen(path, "rb");
    g_orig_len = f ? (long)fread(g_orig, 1, sizeof g_orig, f) : -1;
    if (f) fclose(f);
}
static void write_initial(int round)
{
    int i;
    for (i = 0; i < NNAME; ++i) unlink(g_names[i]);
    if (round != R_FRESH) write_journal(JOURNAL, round);
}

/* ---- the journal's open streams (child) ---- */
typedef struct { FILE *f; int fd, name; } open_file;
static open_file       g_of[16];
static pthread_mutex_t g_ofm = PTHREAD_MUTEX_INITIALIZER;
static void of_add(FILE *f, int name)
{
    int i;
    pthread_mutex_lock(&g_ofm);
    for (i = 0; i < 16 && g_of[i].f; ++i) { }
    if (i == 16) child_bad("more open journal streams than g_of holds");
    g_of[i].f = f; g_of[i].fd = fileno(f); g_of[i].name = name;
    pthread_mutex_unlock(&g_ofm);
}
static void of_del(FILE *f)
{
    int i;
    pthread_mutex_lock(&g_ofm);
    for (i = 0; i < 16; ++i) if (g_of[i].f == f) g_of[i].f = NULL;
    pthread_mutex_unlock(&g_ofm);
}
static int of_name(FILE *f, int fd)            /* by stream, or by descriptor with f NULL */
{
    int i, nm = -1;
    pthread_mutex_lock(&g_ofm);
    for (i = 0; i < 16; ++i)
        if (g_of[i].f && (f ? g_of[i].f == f : g_of[i].fd == fd)) { nm = g_of[i].name; break; }
    pthread_mutex_unlock(&g_ofm);
    return nm;
}

/* ---- the power model (child) ----
 * What survives a power cut: a file's bytes as its last fsync saw them, under the names
 * the directory's last fsync saw. Each file is followed through a read-only descriptor
 * held for the rest of the run - which also keeps its inode number from being reused
 * by the next file, the identity the model goes by. */
static pthread_mutex_t g_mm = PTHREAD_MUTEX_INITIALIZER;
static int             g_rd[NFILE];

static int model_find(uint64_t ino)
{
    uint32_t i;
    for (i = 0; i < S->nfile; ++i) if (S->file[i].ino == ino) return (int)i;
    return -1;
}
static int model_add(int rd)                  /* takes rd; caller holds g_mm */
{
    struct stat st;
    int i;
    if (rd < 0 || fstat(rd, &st) != 0) child_bad("the power model lost a file");
    if ((i = model_find((uint64_t)st.st_ino)) >= 0) { close(rd); return i; }
    if (S->nfile == NFILE) child_bad("more files than NFILE");
    i = (int)S->nfile;
    S->file[i].ino = (uint64_t)st.st_ino;
    S->file[i].pub = 0;
    g_rd[i] = rd;
    S->nfile += 1;
    return i;
}
static int model_follow(int fd)               /* caller holds g_mm */
{
    char p[64];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    return model_add(open(p, O_RDONLY));
}
static void model_sync_file(int i)            /* caller holds g_mm */
{
    file_model *m = &S->file[i];
    uint32_t spare = m->pub == 1 ? 1u : 0u;
    struct stat st;
    ssize_t n;
    if (fstat(g_rd[i], &st) != 0 || st.st_size > IMG_MAX) child_bad("a journal file outgrew IMG_MAX");
    n = pread(g_rd[i], m->img[spare], IMG_MAX, 0);
    if (n < 0) child_bad("the power model could not read a file back");
    m->len[spare] = (uint32_t)n;
    st32(&m->pub, spare + 1);
}
static void model_sync_dir(void)              /* caller holds g_mm */
{
    uint32_t spare = S->dir_pub == 1 ? 1u : 0u;
    int k;
    for (k = 0; k < NNAME; ++k) {
        struct stat st;
        int i = -1;
        if (stat(g_names[k], &st) == 0 && (i = model_find((uint64_t)st.st_ino)) < 0)
            i = model_add(open(g_names[k], O_RDONLY));
        S->dir[spare][k] = i + 1;
    }
    st32(&S->dir_pub, spare + 1);
}
static void model_start(void)                 /* what the parent left is durable */
{
    int k;
    pthread_mutex_lock(&g_mm);
    for (k = 0; k < NNAME; ++k) {
        int fd = open(g_names[k], O_RDONLY);
        if (fd >= 0) model_sync_file(model_add(fd));
    }
    model_sync_dir();
    pthread_mutex_unlock(&g_mm);
}

/* The parent, once the child is gone: put back what the power cut would have left. */
static void power_loss(void)
{
    uint32_t dp = S->dir_pub;
    int k;
    for (k = 0; k < NNAME; ++k) unlink(g_names[k]);
    for (k = 0; k < NNAME; ++k) {
        int i = dp ? S->dir[dp - 1][k] : 0, fd;
        const file_model *m;
        if (!i) continue;
        m = &S->file[i - 1];
        fd = open(g_names[k], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0);
        if (fd < 0) continue;
        if (m->pub) CHECK(write(fd, m->img[m->pub - 1], m->len[m->pub - 1]) == (ssize_t)m->len[m->pub - 1]);
        close(fd);
    }
}

/* ---- the wrappers ----
 * In the child, a call on one of the journal's files is counted; the one the plan
 * names is acted on. Everything else - and everything in the parent - passes through,
 * except that the parent's fsyncs are skipped: its files are scratch. */
static uint32_t g_rng;
static uint32_t rnd(void) { uint32_t x = g_rng ? g_rng : 1u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return g_rng = x; }
static size_t pick(size_t len)                /* how much of a write reaches the file: 0 .. len-1 */
{
    size_t edge[6];
    uint32_t r = rnd();
    edge[0] = 0; edge[1] = 1; edge[2] = DQ_RHDR_LEN - 1; edge[3] = DQ_RHDR_LEN;
    edge[4] = len > 4 ? len - 4 : 0; edge[5] = len - 1;   /* all but the checksum, all but a byte */
    if (r % 3 == 0) { size_t e = edge[(r / 3) % 6]; return e < len ? e : len - 1; }
    return (size_t)(rnd() % (uint32_t)len);
}
static uint32_t io_count(int io)
{
    uint32_t n = __atomic_add_fetch(&S->events, 1, __ATOMIC_SEQ_CST);
    if (n <= NEV) S->ev_io[n - 1] = (uint8_t)io;
    __atomic_add_fetch(&S->io_seen[io], 1, __ATOMIC_SEQ_CST);
    return n;
}
static void note_hit(uint32_t n, int io, int name)
{
    S->hit_io = io; S->hit_name = name;
    st32(&S->hit, n);
}
/* The crash: SIGKILL, as the OOM killer sends it. Nothing of the process runs after it -
 * no atexit, no stdio flush, and no sanitizer's report on the threads it stops. */
static void die(void)
{
    kill(getpid(), SIGKILL);
    for (;;) pause();
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    int nm, io;
    uint32_t n;
    FILE *f;
    if ((nm = name_index(path)) < 0) return __real_fopen(path, mode);
    io = (mode[0] == 'r' && !strchr(mode, '+')) ? IO_OPEN_R : IO_OPEN_W;
    if (!g_child) {
        /* A single case (below) can make the parent's opening for writing of one name
         * fail, once, after letting g_pfail_skip of them through. */
        if (nm == g_pfail_name && io == IO_OPEN_W && g_pfail_skip-- == 0) {
            g_pfail_name = -1;
            errno = ENOSPC;
            return NULL;
        }
        return __real_fopen(path, mode);
    }
    n = io_count(io);
    if (n == S->at) {
        note_hit(n, io, nm);
        if (S->mode == M_BEFORE) die();
        if (failing()) { errno = EIO; return NULL; }
    }
    f = __real_fopen(path, mode);
    if (f) {
        of_add(f, nm);
        pthread_mutex_lock(&g_mm); model_follow(fileno(f)); pthread_mutex_unlock(&g_mm);
    }
    if (n == S->at) die();
    return f;
}

size_t __wrap_fread(void *p, size_t size, size_t cnt, FILE *f)
{
    int nm;
    uint32_t n;
    size_t r;
    if (!g_child || !f || (nm = of_name(f, -1)) < 0) return __real_fread(p, size, cnt, f);
    n = io_count(IO_READ);
    if (n == S->at) {
        note_hit(n, IO_READ, nm);
        if (S->mode == M_BEFORE) die();
        if (failing()) {
            /* A read error leaves the stream's error indicator set. Writing to a stream
             * opened for reading is the portable way to set it: that write fails. */
            (void)__real_fwrite("", 1, 1, f);
            if (!ferror(f)) child_bad("could not set a read stream's error indicator");
            errno = EIO;
            return 0;
        }
    }
    r = __real_fread(p, size, cnt, f);
    if (n == S->at) die();
    return r;
}

size_t __wrap_fwrite(const void *p, size_t size, size_t cnt, FILE *f)
{
    int nm;
    uint32_t n;
    size_t w;
    if (!g_child || !f || (nm = of_name(f, -1)) < 0) return __real_fwrite(p, size, cnt, f);
    n = io_count(IO_WRITE);
    if (n == S->at) {
        size_t len = size * cnt, cut;
        note_hit(n, IO_WRITE, nm);
        S->hit_len = (uint32_t)len;
        if (S->mode == M_BEFORE) die();
        if ((S->mode == M_TORN || failing()) && len) {
            cut = pick(len);
            S->hit_cut = (uint32_t)cut;
            if (S->mode == M_TORN) {
                /* The crash came while these bytes went to the file: whatever stdio
                 * held before them got there, and a prefix of them. */
                ssize_t r;
                __real_fflush(f);
                r = cut ? write(fileno(f), p, cut) : 0;
                (void)r;
                die();
            }
            w = cut ? __real_fwrite(p, 1, cut, f) : 0;    /* a short write: the disk filled */
            errno = ENOSPC;
            return size ? w / size : 0;
        }
    }
    w = __real_fwrite(p, size, cnt, f);
    if (n == S->at) die();
    return w;
}

int __wrap_fflush(FILE *f)
{
    int nm, rc;
    uint32_t n;
    if (!g_child || !f || (nm = of_name(f, -1)) < 0) return __real_fflush(f);
    n = io_count(IO_FLUSH);
    if (n == S->at) {
        note_hit(n, IO_FLUSH, nm);
        if (S->mode == M_BEFORE) die();
        if (failing()) { errno = EIO; return EOF; }
    }
    rc = __real_fflush(f);
    if (n == S->at) die();
    return rc;
}

int __wrap_fclose(FILE *f)
{
    int nm, rc;
    uint32_t n;
    if (!g_child || !f || (nm = of_name(f, -1)) < 0) return __real_fclose(f);
    n = io_count(IO_CLOSE);
    if (n == S->at) {
        note_hit(n, IO_CLOSE, nm);
        if (S->mode == M_BEFORE) die();
    }
    of_del(f);
    rc = __real_fclose(f);             /* a failed fclose still closes the stream */
    if (n == S->at) {
        if (failing()) { errno = EIO; return EOF; }
        die();
    }
    return rc;
}

int __wrap_fsync(int fd)
{
    struct stat st;
    int nm = -1, io, i;
    uint32_t n;
    if (!g_child) return 0;
    if (fstat(fd, &st) != 0) return __real_fsync(fd);
    if (S_ISDIR(st.st_mode)) {
        if (st.st_dev != g_cwd_dev || st.st_ino != g_cwd_ino) return __real_fsync(fd);
        io = IO_DIRSYNC;
    } else {
        if ((nm = of_name(NULL, fd)) < 0) return __real_fsync(fd);
        io = IO_SYNC;
    }
    n = io_count(io);
    if (S->mode == M_ERRSYNC && ld32(&S->hit) && n > S->at) die();   /* the power fails first */
    if (n == S->at) {
        note_hit(n, io, nm);
        if (S->mode == M_BEFORE) die();
        if (failing()) { errno = EIO; return -1; }
    }
    if (S->mode == M_ERR2 && ld32(&S->hit) && n > S->at &&
        (io == IO_DIRSYNC) == (S->hit_io == IO_DIRSYNC)) {
        uint32_t none = 0;              /* the second error: once, on the first that comes */
        if (__atomic_compare_exchange_n(&S->hit2, &none, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            sti32(&S->hit2_io, io);
            errno = EIO;
            return -1;
        }
    }
    if (io == IO_DIRSYNC && S->dir_einval) { errno = EINVAL; return -1; }   /* cannot sync one */
    if (io == IO_SYNC && g_sync_delay_us > 0) {
        struct timespec ts;
        ts.tv_sec = g_sync_delay_us / 1000000; ts.tv_nsec = (g_sync_delay_us % 1000000) * 1000;
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&g_mm);
    if (io == IO_DIRSYNC) model_sync_dir();
    else {
        if ((i = model_find((uint64_t)st.st_ino)) < 0) i = model_follow(fd);
        model_sync_file(i);
    }
    pthread_mutex_unlock(&g_mm);
    if (n == S->at) die();
    return 0;
}

int __wrap_ftruncate(int fd, off_t len)
{
    int nm, rc;
    uint32_t n;
    if (!g_child || (nm = of_name(NULL, fd)) < 0) return __real_ftruncate(fd, len);
    n = io_count(IO_TRUNC);
    if (n == S->at) {
        note_hit(n, IO_TRUNC, nm);
        if (S->mode == M_BEFORE) die();
        if (failing()) { errno = EIO; return -1; }
    }
    rc = __real_ftruncate(fd, len);
    if (n == S->at) die();
    return rc;
}

int __wrap_rename(const char *from, const char *to)
{
    int nm, rc;
    uint32_t n;
    if (!g_child || ((nm = name_index(to)) < 0 && name_index(from) < 0)) return __real_rename(from, to);
    n = io_count(IO_RENAME);
    if (n == S->at) {
        note_hit(n, IO_RENAME, nm);
        if (S->mode == M_BEFORE) die();
        if (failing()) { errno = EIO; return -1; }
    }
    rc = __real_rename(from, to);
    if (n == S->at) die();
    return rc;
}

int __wrap_remove(const char *path)
{
    int nm, rc;
    uint32_t n;
    if (!g_child || (nm = name_index(path)) < 0) return __real_remove(path);
    n = io_count(IO_REMOVE);
    if (n == S->at) {
        note_hit(n, IO_REMOVE, nm);
        if (S->mode == M_BEFORE) die();
        if (failing()) { errno = EIO; return -1; }
    }
    rc = __real_remove(path);
    if (n == S->at) die();
    return rc;
}

/* ---- task bodies, shared by the child and the parent's verification ---- */
static gptps    *g_e;
static gptps_dq *g_dq;
static int       g_vran[NID];         /* parent: runs of each id while verifying */

static int run_id(gptps_ctx *ctx)
{
    size_t n = 0;
    const void *p = gptps_payload(ctx, &n);
    int id = id_of(p, n);
    if (!g_child && id > 0) ++g_vran[id];
    return id;
}
static void do_compact(void);
static gptps_status body_ok(gptps_ctx *ctx, void *ud)   { (void)ud; run_id(ctx); return GPTPS_OK; }
static gptps_status body_fail(gptps_ctx *ctx, void *ud) { (void)ud; run_id(ctx); return GPTPS_E_TASK; }
static gptps_status body_self(gptps_ctx *ctx, void *ud) { (void)ud; run_id(ctx); return GPTPS_E_CANCELLED; }
static gptps_status body_compactor(gptps_ctx *ctx, void *ud)
{
    (void)ud;
    run_id(ctx);
    if (g_child) do_compact();         /* a compaction while this attempt runs: its 'S' must survive it */
    else gptps_dq_compact(g_dq);
    return GPTPS_OK;
}

/* MANUAL, one at a time: the order things happen in is the order this file says. */
static gptps *open_engine(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    gptps_run_fn run[K_NOSUCH] = { body_ok, body_fail, body_self, body_compactor };
    int k;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 64u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return NULL;
    for (k = 0; k < K_NOSUCH; ++k) {   /* "nosuch" is never registered */
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = kind_name[k]; d.run = run[k]; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;     /* dead_letter, no retries */
        if (gptps_register_task(e, &d) != GPTPS_OK) { gptps_shutdown(e); return NULL; }
    }
    return e;
}

/* ---- the child: the workload ---- */
static gptps_handle    g_h[NID];
static pthread_mutex_t g_hm = PTHREAD_MUTEX_INITIALIZER;
static void set_handle(int id, gptps_handle h) { pthread_mutex_lock(&g_hm); g_h[id] = h; pthread_mutex_unlock(&g_hm); }
static gptps_handle handle_of(int id)
{
    gptps_handle h;
    pthread_mutex_lock(&g_hm); h = g_h[id]; pthread_mutex_unlock(&g_hm);
    return h;
}
static int id_of_handle(gptps_handle h)
{
    int i, id = -1;
    pthread_mutex_lock(&g_hm);
    for (i = 1; i < NID && h; ++i) if (g_h[i] == h) { id = i; break; }
    pthread_mutex_unlock(&g_hm);
    return id;
}

/* Until something has been injected, the workload must go exactly as written. */
static void expect(int ok, const char *what) { if (!ok && !failing()) child_bad(what); }

static void quiet_sink(gptps_log_level lvl, const char *msg, void *ud) { (void)lvl; (void)msg; (void)ud; }

/* Registered after gptps_dq_open, so it runs before the queue's observer; post_obs,
 * registered before, runs after it (the engine calls the newest observer first).
 * Between the two, the queue is writing a marker for the record, and a crash leaves
 * it written or not. */
static void pre_obs(const gptps_event *ev, void *ud)
{
    int id, closing;
    (void)ud;
    if (ev->kind == GPTPS_EV_QUEUED || ev->kind == GPTPS_EV_RETRIED) return;
    if ((id = id_of_handle(ev->handle)) <= 0) return;
    closing = ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DROPPED ||
              (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED &&
               has_flag(ev, GPTPS_EV_FLAG_SELF_CANCELLED));
    st32(&S->id[id].busy_close, (uint32_t)closing);
    st32(&S->id[id].busy, tick());
}
static void post_obs(const gptps_event *ev, void *ud)
{
    fact *f;
    int id;
    (void)ud;
    if (ev->kind == GPTPS_EV_QUEUED || ev->kind == GPTPS_EV_RETRIED) return;
    if ((id = id_of_handle(ev->handle)) <= 0) return;
    f = &S->id[id];
    switch (ev->kind) {
    case GPTPS_EV_STARTED:
        st32(&f->start, tick());
        break;
    case GPTPS_EV_FINISHED:
        st32(&f->end, tick()); st32(&f->closed, tick());
        break;
    case GPTPS_EV_FAILED:
        if (ld32(&f->start) > ld32(&f->end)) st32(&f->end, tick());
        if (ev->status == GPTPS_E_CANCELLED && has_flag(ev, GPTPS_EV_FLAG_SELF_CANCELLED))
            st32(&f->closed, tick());
        break;
    case GPTPS_EV_DEAD_LETTERED:
        if (!has_flag(ev, GPTPS_EV_FLAG_SHUTDOWN)) st32(&f->quar, tick());
        break;
    case GPTPS_EV_DROPPED:
        if (!has_flag(ev, GPTPS_EV_FLAG_SHUTDOWN)) st32(&f->closed, tick());
        break;
    default:
        break;
    }
    st32(&f->busy, 0);
}

static void resub_cb(const char *name, const void *payload, size_t len, gptps_handle h, void *ud)
{
    int id = id_of(payload, len);
    (void)name; (void)ud;
    if (id <= 0) child_bad("gptps_dq_recover handed back a record nobody wrote");
    set_handle(id, h);
}

/* Is the queue broken (see "a broken queue" in the add-on)? Asked as a durable call
 * returns GPTPS_E_IO: a write that failed as it broke may yet come back. */
static uint32_t queue_broken(void)
{
    int b;
    apx_mutex_lock(&g_dq->jmu);
    b = g_dq->broken;
    apx_mutex_unlock(&g_dq->jmu);
    if (b) st32(&S->broke, 1);
    return b ? 1u : 0u;
}

static void do_submit(int id)
{
    unsigned char pl[PL_MAX];
    size_t n = make_payload(id, pl);
    gptps_handle h = 0;
    gptps_status st;
    st32(&S->id[id].sub_call, tick());
    st = gptps_dq_submit(g_dq, kind_name[kind_of(id)], pl, n, &h);
    if (st == GPTPS_OK) set_handle(id, h);
    if (st == GPTPS_E_IO) st32(&S->id[id].sub_broken, queue_broken());
    sti32(&S->id[id].sub_st, (int32_t)st);
    st32(&S->id[id].sub_ret, tick());
    expect(st == (kind_of(id) == K_NOSUCH ? GPTPS_E_NOTFOUND : GPTPS_OK), "gptps_dq_submit");
}

static void do_batch(int first, int n)  /* n <= 4 */
{
    gptps_dq_item it[4];
    gptps_status st;
    uint32_t broken = 0;
    int i;
    for (i = 0; i < n; ++i) {
        unsigned char *b = (unsigned char *)malloc(PL_MAX);
        if (!b) child_bad("malloc");
        it[i].task_name = kind_name[kind_of(first + i)];
        it[i].len = make_payload(first + i, b);
        it[i].payload = b;
        st32(&S->id[first + i].sub_call, tick());
    }
    st = gptps_dq_submit_batch(g_dq, it, (size_t)n);
    expect(st == GPTPS_OK, "gptps_dq_submit_batch");
    if (st == GPTPS_E_IO) broken = queue_broken();
    for (i = 0; i < n; ++i) {
        if (it[i].status == GPTPS_OK) set_handle(first + i, it[i].handle);
        st32(&S->id[first + i].sub_broken, broken);
        sti32(&S->id[first + i].sub_st, (int32_t)it[i].status);
        st32(&S->id[first + i].sub_ret, tick());
        expect(it[i].status == (kind_of(first + i) == K_NOSUCH ? GPTPS_E_NOTFOUND : GPTPS_OK),
               "a batch item");
        free((void *)it[i].payload);
    }
}

static void do_cancel(int id)
{
    gptps_handle h = handle_of(id);
    gptps_status st;
    if (!h) return;                    /* never got a handle: nothing to retract */
    st32(&S->id[id].can_call, tick());
    st = gptps_dq_cancel(g_dq, h);
    if (st == GPTPS_E_IO) st32(&S->id[id].can_broken, queue_broken());
    sti32(&S->id[id].can_st, (int32_t)st);
    st32(&S->id[id].can_ret, tick());
    expect(st == GPTPS_OK, "gptps_dq_cancel");
}

static void do_compact(void)
{
    uint32_t i = __atomic_fetch_add(&S->ncomp, 1, __ATOMIC_SEQ_CST);
    gptps_status st;
    if (i >= NCOMP) child_bad("more compactions than NCOMP");
    st32(&S->comp[i].call, tick());
    st = gptps_dq_compact(g_dq);
    sti32(&S->comp[i].st, (int32_t)st);
    st32(&S->comp[i].ret, tick());
    expect(st == GPTPS_OK, "gptps_dq_compact");
}

static void drain_cb(const char *name, const void *payload, size_t len, void *ud)
{
    int id = id_of(payload, len);
    (void)name; (void)ud;
    if (id <= 0) child_bad("the drain handed over a record nobody wrote");
    st32(&S->id[id].drained, tick());
}
static void do_drain(void)
{
    gptps_status cst = GPTPS_E_INVAL;
    size_t n;
    st32(&S->drain_call, tick());
    n = gptps_dq_drain_quarantine_ex(g_dq, drain_cb, NULL, &cst);
    st32(&S->drain_n, (uint32_t)n);
    sti32(&S->drain_st, (int32_t)cst);
    st32(&S->drain_ret, tick());
    expect(n == drained_at_end(S->round) && cst == GPTPS_OK, "gptps_dq_drain_quarantine_ex");
}

static void steps(int k)               /* run k attempts; k < 0: until none is left */
{
    size_t ran = 0;
    while (k-- != 0 && gptps_step(g_e, &ran) == GPTPS_OK && ran) { }
}

static void *submitter(void *arg)      /* three of these at once share fsyncs */
{
    int base = (int)(intptr_t)arg;
    do_submit(base);
    do_submit(base + 1);
    do_batch(base + 2, 2);
    return NULL;
}

static void child_main(void)
{
    gptps_dq *dq;
    pthread_t th[3];
    size_t i;
    int t;
    g_child = 1;
    g_rng = S->seed * 2654435761u ^ S->at * 40503u ^ (uint32_t)S->round * 97u ^ (uint32_t)S->group * 7919u;
    alarm(20);                          /* a hang shows as SIGALRM in the parent */
    gptps_set_log_sink(quiet_sink, NULL);
    model_start();
    g_e = open_engine();
    if (!g_e) child_bad("gptps_open_ex");
    gptps_register_observer(g_e, post_obs, NULL);
    dq = gptps_dq_open(g_e, JOURNAL);
    st32(&S->open_events, ld32(&S->events));
    sti32(&S->open_ok, dq != NULL);
    st32(&S->open_ret, tick());
    if (!dq) { expect(0, "gptps_dq_open"); _exit(EXIT_END); }
    g_dq = dq;
    gptps_register_observer(g_e, pre_obs, NULL);
    for (i = 0; i < dq->n; ++i) {       /* what this open counted, to check the next one by */
        const dq_rec *rc = &dq->recs[i];
        int id = rc->done ? -1 : id_of(rc->payload, rc->len);
        if (id > 0) st32(&S->id[id].k_open, rc->crashes + 1);
    }
    gptps_dq_set_resubmit_cb(dq, resub_cb, NULL);
    expect(gptps_dq_recover(dq) == recovers_at_start(S->round), "gptps_dq_recover");

    for (t = 20; t <= 25; ++t) do_submit(t);
    do_batch(26, 4);                    /* 27 is refused: closed by a marker */
    steps(3);                           /* three attempts start and finish */
    do_cancel(23);                      /* retracted while it waits */
    gptps_cancel(g_e, handle_of(24));   /* stopped, not retracted: it stays pending */
    do_compact();
    do_submit(30);
    steps(-1);                          /* the rest, the suspects one at a time, a compaction inside one */
    do_drain();
    do_submit(31);

    if (S->group) {
        for (t = 0; t < 3; ++t)
            if (pthread_create(&th[t], NULL, submitter, (void *)(intptr_t)(40 + 4 * t)) != 0)
                child_bad("pthread_create");
        do_cancel(31);                  /* while they submit */
        do_compact();
        for (t = 0; t < 3; ++t) pthread_join(th[t], NULL);
    } else {
        do_cancel(31);
        do_compact();
    }
    do_submit(60);                      /* the last word: still journaling */
    _exit(EXIT_END);
}

/* ---- the parent: recover, and judge ---- */
typedef struct { uint64_t seq; int id; int quar; uint32_t crashes; int suspect; } got;

static int           g_warns, g_shown, g_dumped;
static char          g_warn[1024];      /* the last damage warning */
static char          g_ctx[256];
static unsigned char g_left[IMG_MAX];   /* the journal as the crash left it, for a failure to show */
static long          g_left_len;
static void count_sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl >= GPTPS_LOG_WARN && msg && strstr(msg, " is damaged")) {
        ++g_warns;
        snprintf(g_warn, sizeof g_warn, "%s", msg);
    }
}
static void keep_left(void)
{
    FILE *f = fopen(JOURNAL, "rb");
    g_left_len = f ? (long)fread(g_left, 1, sizeof g_left, f) : -1;
    if (f) fclose(f);
    g_dumped = 0;
}
/* The records of the journal the crash left: type, seq, and a 'P''s id - "P12#20" is
 * record 20 at seq 12. Diagnostics only: it stops at the first header it cannot read. */
static void dump_left(void)
{
    long at = DQ_FHDR_LEN;
    printf("  the journal the crash left (%ld bytes):", g_left_len);
    while (at + DQ_RHDR_LEN + 4 <= g_left_len && get32(g_left + at) == DQ_REC_MAGIC) {
        const unsigned char *r = g_left + at;
        unsigned nlen = get16(r + 6);
        unsigned long plen = (unsigned long)get32(r + 8);
        if (at + DQ_RHDR_LEN + (long)nlen + (long)plen + 4 > g_left_len) { printf(" (torn)"); break; }
        printf(" %c%llu", r[4] >= 32 && r[4] < 127 ? r[4] : '?', (unsigned long long)get64(r + 12));
        if (r[4] == 'P') printf("#%d", id_of(r + DQ_RHDR_LEN + nlen, plen));
        if (r[4] == 'K' && plen == 4) printf("=%u", (unsigned)get32(r + DQ_RHDR_LEN));
        at += DQ_RHDR_LEN + (long)nlen + (long)plen + 4;
    }
    if (at < g_left_len && at + DQ_RHDR_LEN + 4 > g_left_len) printf(" (+%ld bytes)", g_left_len - at);
    printf("\n");
}
static void verdict(const char *fmt, ...)
{
    va_list ap;
    ++fails;
    if (g_shown++ >= SHOW_MAX) { if (g_shown == SHOW_MAX + 1) printf("FAIL ... (more not shown)\n"); return; }
    printf("FAIL %s: ", g_ctx);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (!g_dumped++) dump_left();
}
static void on_alarm(int sig)
{
    static const char msg[] = "FAIL: the parent's recovery hung: ";
    ssize_t r;
    (void)sig;
    r = write(1, msg, sizeof msg - 1);
    r = write(1, g_ctx, strlen(g_ctx));
    r = write(1, "\n", 1);
    (void)r;
    _exit(1);
}

/* Every open record a recovery loaded: read from its table, as gptps_dq_recover would. */
static int collect(gptps_dq *dq, got *g)
{
    size_t i;
    int n = 0;
    apx_mutex_lock(&dq->mu);
    for (i = 0; i < dq->n && n < NGOT; ++i) {
        const dq_rec *rc = &dq->recs[i];
        int id;
        if (rc->done) continue;
        id = id_of(rc->payload, rc->len);
        if (id > 0 && strcmp(rc->name ? rc->name : "", kind_name[kind_of(id)]) != 0) id = -1;
        if (id < 0)
            verdict("recovered a record nobody wrote: seq %llu, task '%s', %lu bytes",
                    (unsigned long long)rc->seq, rc->name ? rc->name : "?", (unsigned long)rc->len);
        g[n].seq = rc->seq; g[n].id = id; g[n].quar = rc->quarantined;
        g[n].crashes = rc->crashes; g[n].suspect = rc->suspect;
        ++n;
    }
    apx_mutex_unlock(&dq->mu);
    return n;
}

/* A record closed at tick t is gone from the journal once a compaction that started
 * after t returned GPTPS_OK: it rewrote the journal without it. The drain compacts too. */
static int compacted_after(uint32_t t)
{
    uint32_t i;
    for (i = 0; i < S->ncomp && i < NCOMP; ++i)
        if (S->comp[i].call > t && S->comp[i].ret && S->comp[i].st == GPTPS_OK) return 1;
    return S->drain_n && S->drain_call > t && S->drain_ret && S->drain_st == GPTPS_OK;
}

enum { MAY, MUST, MUST_NOT };
static int expect_of(int round, int id, const char **why)
{
    const fact *f = &S->id[id];
    int init = init_state(round, id);
    if (init == I_DONE) { *why = "closed before this run"; return MUST_NOT; }
    if (init == I_LOST) { *why = "damaged before this run"; return MUST_NOT; }
    if (init == I_CUT)  { *why = "after a cut: kept in the copy, not loaded"; return MUST_NOT; }
    if (init == I_NONE) {
        if (!f->sub_call) { *why = "never submitted"; return MUST_NOT; }
        if (!f->sub_ret)  { *why = "its submit never returned"; return MAY; }
        if (f->sub_st == GPTPS_E_IO) {
            /* GPTPS_E_IO: the queue made sure the record cannot come back - unless it
             * broke doing so, and no compaction has repaired it since. */
            if (f->sub_broken && !compacted_after(f->sub_ret)) {
                *why = "its submit failed as the queue broke"; return MAY;
            }
            *why = "its submit returned GPTPS_E_IO"; return MUST_NOT;
        }
        if (f->sub_st != GPTPS_OK) {   /* refused by the engine: closed by a marker, not fsync'd */
            *why = "refused by the engine";
            return compacted_after(f->sub_ret) ? MUST_NOT : MAY;
        }
    }
    /* acknowledged, in this run or a run before */
    if (f->can_ret && (f->can_st == GPTPS_OK || f->can_st == GPTPS_E_SHUTDOWN)) { *why = "retracted"; return MUST_NOT; }
    if (f->can_call && !f->can_ret) { *why = "its retraction never returned"; return MAY; }
    if (f->can_ret && f->can_st == GPTPS_E_IO && f->can_broken && !compacted_after(f->can_ret)) {
        *why = "its retraction failed as the queue broke"; return MAY;
    }
    if (f->busy && f->busy_close)   { *why = "the crash came while its close was journaled"; return MAY; }
    if (f->drained) {
        *why = "drained";
        return (S->drain_ret && S->drain_st == GPTPS_OK) || compacted_after(f->drained) ? MUST_NOT : MAY;
    }
    if (f->quar || init == I_QUAR) { *why = "dead-lettered"; return MUST; }
    if (f->closed) { *why = "closed"; return compacted_after(f->closed) ? MUST_NOT : MAY; }
    *why = "acknowledged";
    return MUST;
}

static void judge(int round, int mode, const got *g, int n)
{
    int id, i;
    for (id = 1; id < NID; ++id) {
        const char *why = "";
        int cnt = 0, e = expect_of(round, id, &why);
        for (i = 0; i < n; ++i) cnt += g[i].id == id;
        if (cnt > 1) verdict("record %d is in the journal %d times", id, cnt);
        if (e == MUST && !cnt) verdict("record %d (%s) was lost", id, why);
        if (e == MUST_NOT && cnt) verdict("record %d (%s) came back", id, why);
    }
    /* A process that dies keeps what it wrote, so the attempt markers say exactly which
     * records were running at the death. A power cut, or an injected error's rollback,
     * may take markers with it, which the header allows: no count is checked then. */
    if (mode != M_BEFORE && mode != M_AFTER && mode != M_TORN) return;
    for (i = 0; i < n; ++i) {
        const fact *f;
        uint32_t c0, want;
        if ((id = g[i].id) <= 0) continue;
        f = &S->id[id];
        if (f->busy) continue;          /* its marker may or may not have got there */
        c0 = init_crashes(round, id);
        if (f->k_open && f->k_open - 1 != c0)
            verdict("record %d: the child's gptps_dq_open counted %u crashes, want %u", id, f->k_open - 1, c0);
        want = (f->end ? 0 : c0) + (f->start > f->end ? 1 : 0);
        if (g[i].crashes != want)
            verdict("record %d: %u crashes counted, want %u", id, g[i].crashes, want);
    }
}

static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static int file_is(const char *path, const void *bytes, long len)   /* holds exactly these */
{
    static unsigned char buf[IMG_MAX];
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return 0;
    n = (long)fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n == len && memcmp(buf, bytes, (size_t)len) == 0;
}

/* Is the journal a damaged round started from kept, byte for byte, as one of the
 * ".corrupt" copies? Recovery compacts the damage away, and the header promises the
 * original is copied first - for a cut, that copy is the only place the records after
 * the cut still are. */
static int copy_kept(void)
{
    static unsigned char buf[IMG_MAX];
    int k;
    for (k = 2; k < NNAME; ++k) {
        FILE *f = fopen(g_names[k], "rb");
        long n;
        if (!f) continue;
        n = (long)fread(buf, 1, sizeof buf, f);
        fclose(f);
        if (n == g_orig_len && memcmp(buf, g_orig, (size_t)n) == 0) return 1;
    }
    return 0;
}

static void verify(int round, int mode)
{
    got a[NGOT], b[NGOT];
    int na, nb, i, nosuch = 0;
    gptps *e;
    gptps_dq *dq;
    size_t ran = 0;
    alarm(30);
    /* A journal the child's open created is not gone either - unless the power failed
     * before any sync of the directory made its name durable: nothing it held was
     * acknowledged then (the group commit syncs the directory first), as judge checks. */
    if ((round != R_FRESH || (S->open_ok && !powered(mode))) && !exists(JOURNAL))
        verdict("the journal is gone");
    /* "Returns NULL, and leaves the journal as it was". */
    if (S->open_ret && !S->open_ok && (round == R_FRESH ? exists(JOURNAL) : !file_is(JOURNAL, g_orig, g_orig_len)))
        verdict("gptps_dq_open returned NULL, and did not leave the journal as it was");
    g_warns = 0;
    e = open_engine(); CHECK(e != NULL); if (!e) return;
    dq = gptps_dq_open(e, JOURNAL);
    if (!dq) { verdict("gptps_dq_open failed on what the crash left"); gptps_shutdown(e); alarm(0); return; }
    na = collect(dq, a);
    gptps_shutdown(e);                  /* never stepped, nothing submitted: no events */
    gptps_dq_close(dq);
    if (round != R_DAMAGED && round != R_CUT && g_warns) verdict("a crash's journal was reported as damaged");
    /* A journal cut short is compacted only once it is copied: the copy is the only place
     * the records after the cut are kept. With confined damage the open goes on without
     * one if it cannot make one - here, only when a call of the child's open failed. */
    if (round == R_CUT && !copy_kept())
        verdict("the journal cut short was compacted, and no .corrupt copy of it is left");
    if (round == R_DAMAGED && !copy_kept() && !(mode >= M_ERROR && S->hit && S->hit <= S->open_events))
        verdict("the damaged journal was compacted, and no .corrupt copy of it is left");
    judge(round, mode, a, na);

    /* Recover again from what the first recovery left: the same records, the same counts. */
    e = open_engine(); CHECK(e != NULL); if (!e) return;
    dq = gptps_dq_open(e, JOURNAL);
    if (!dq) { verdict("the second gptps_dq_open failed"); gptps_shutdown(e); alarm(0); return; }
    nb = collect(dq, b);
    if (nb != na) verdict("the second recovery loaded %d records, the first %d", nb, na);
    for (i = 0; i < na && i < nb; ++i)
        if (a[i].seq != b[i].seq || a[i].id != b[i].id || a[i].quar != b[i].quar ||
            a[i].crashes != b[i].crashes || a[i].suspect != b[i].suspect)
            verdict("the second recovery differs at record %d (seq %llu)", a[i].id, (unsigned long long)a[i].seq);

    /* And everything it recovered runs, once - the suspects one at a time. */
    memset(g_vran, 0, sizeof g_vran);
    g_dq = dq;
    gptps_dq_recover(dq);
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
    for (i = 0; i < nb; ++i) {
        int want = !b[i].quar && kind_of(b[i].id) != K_NOSUCH;
        if (b[i].id <= 0) continue;
        nosuch += !b[i].quar && kind_of(b[i].id) == K_NOSUCH;
        if (g_vran[b[i].id] != want)
            verdict("record %d ran %d times after recovery, want %d", b[i].id, g_vran[b[i].id], want);
        g_vran[b[i].id] = 0;
    }
    for (i = 1; i < NID; ++i) if (g_vran[i]) verdict("record %d ran, though nothing recovered it", i);
    if (gptps_dq_pending(dq) != (size_t)nosuch)
        verdict("%lu records still pending once all had run", (unsigned long)gptps_dq_pending(dq));
    gptps_shutdown(e);
    gptps_dq_close(dq);
    g_dq = NULL;
    alarm(0);
}

/* ---- the sweep ---- */
static uint32_t g_io_total[IO_N];
static unsigned g_runs[M_N + 1];
static unsigned g_broke_runs;     /* runs in which the child saw its queue broken */
static unsigned g_dir2_runs;      /* two errors: runs whose second one was a directory's fsync */
static unsigned g_einval_runs;    /* runs with every directory fsync failing with EINVAL */

static void describe(uint32_t seed, int round, int mode, uint32_t at)
{
    char what[96] = "";
    if (S->hit) {
        const char *file = S->hit_name >= 0 ? g_names[S->hit_name] : "the directory";
        if (S->hit_io == IO_WRITE && (mode == M_TORN || mode >= M_ERROR))
            snprintf(what, sizeof what, " (%s %s: %u of %u bytes)", io_name[S->hit_io], file,
                     S->hit_cut, S->hit_len);
        else
            snprintf(what, sizeof what, " (%s %s)", io_name[S->hit_io], file);
    }
    if (at)
        snprintf(g_ctx, sizeof g_ctx, "seed %u, %s journal, %s, %s at call %u%s", seed,
                 round_name[round], S->group ? "three submitters" : "one thread", mode_name[mode],
                 at, S->hit ? what : " (not reached)");
    else
        snprintf(g_ctx, sizeof g_ctx, "seed %u, %s journal, %s, no crash until the end%s", seed,
                 round_name[round], S->group ? "three submitters" : "one thread",
                 S->dir_einval ? ", every fsync of the directory failing with EINVAL" : "");
}

/* One child: run the workload, crash it as planned, and judge what it left. */
static void run_one(uint32_t seed, int round, int group, int mode, uint32_t at, int dir_einval)
{
    pid_t pid;
    int wst = 0, k, planned;
    write_initial(round);
    memset(S, 0, offsetof(shared, nfile) + sizeof S->nfile);
    S->mode = mode; S->at = at; S->seed = seed; S->round = round; S->group = group;
    S->dir_einval = dir_einval;
    fflush(stdout);
    pid = fork();
    CHECK(pid >= 0);
    if (pid < 0) return;
    if (pid == 0) child_main();         /* never returns */
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR) { }
    g_runs[at ? mode : M_N] += 1;
    g_broke_runs += S->broke ? 1u : 0u;
    g_dir2_runs += (S->hit2 && S->hit2_io == IO_DIRSYNC) ? 1u : 0u;
    g_einval_runs += dir_einval ? 1u : 0u;
    for (k = 0; k < IO_N; ++k) g_io_total[k] += S->io_seen[k];
    describe(seed, round, mode, at);
    /* As planned: killed once the chosen call was reached, or at the end of the workload.
     * Anything else - another signal, SIGALRM for a hang, an exit of its own - fails. */
    planned = (WIFSIGNALED(wst) && WTERMSIG(wst) == SIGKILL && ld32(&S->hit)) ||
              (WIFEXITED(wst) && WEXITSTATUS(wst) == EXIT_END);
    if (planned && powered(mode)) power_loss();
    keep_left();
    if (!planned) {
        if (WIFEXITED(wst) && WEXITSTATUS(wst) == EXIT_BAD) verdict("the workload went wrong: %s", S->bad);
        else if (WIFSIGNALED(wst) && WTERMSIG(wst) == SIGALRM) verdict("the child hung");
        else if (WIFSIGNALED(wst)) verdict("the child died of signal %d", WTERMSIG(wst));
        else verdict("the child exited with %d", WIFEXITED(wst) ? WEXITSTATUS(wst) : -1);
        return;
    }
    verify(round, at ? mode : M_AFTER);
}

/* Which calls a way of dying applies to. Reads change nothing, so dying before or after
 * one is dying before the next call that does; tearing needs a write. */
static int applies(int mode, int io)
{
    if (mode >= M_ERROR) return 1;
    if (mode == M_TORN) return io == IO_WRITE;
    return io != IO_READ && io != IO_OPEN_R;
}

/* ---- single cases ----
 * What one failing call in a sweep cannot set up: where the ".corrupt" copies go when
 * every slot is taken or one cannot be read, what an open does with a damaged journal
 * no copy of which can be made, and a directory this process may write but not read.
 * They run in the parent, whose calls the wrappers pass through - but for an opening for
 * writing that g_pfail_name makes fail - and whose fsyncs they skip. */

static void put_file(const char *path, const char *text, time_t mtime)
{
    struct timespec ts[2];
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    CHECK(fwrite(text, 1, strlen(text), f) == strlen(text));
    CHECK(fclose(f) == 0);
    ts[0].tv_sec = ts[1].tv_sec = mtime; ts[0].tv_nsec = ts[1].tv_nsec = 0;
    CHECK(utimensat(AT_FDCWD, path, ts, 0) == 0);
}
static size_t pending_at_start(int round)   /* what gptps_dq_pending says once it opens */
{
    size_t n = 0;
    int id;
    for (id = 1; id < NID; ++id) n += init_state(round, id) == I_PENDING && init_crashes(round, id) < 3;
    return n;
}

/* All ten copy slots taken, by earlier incidents nobody cleaned up after. The queue
 * still opens - it used to fail for ever, when a cut needs the copy - and the copy
 * replaces the OLDEST of them, by modification time, and no other. */
static void case_copies_full(void)
{
    char text[NNAME][48];
    int round, k;
    for (round = R_DAMAGED; round <= R_CUT; ++round) {
        gptps *e = open_engine();
        gptps_dq *dq;
        write_initial(round);
        for (k = 2; k < NNAME; ++k) {
            snprintf(text[k], sizeof text[k], "an earlier incident, number %d", k - 1);
            put_file(g_names[k], text[k], (time_t)(k == 6 ? 1000000000 : 1100000000 + k));
        }
        CHECK(e != NULL); if (!e) return;
        dq = gptps_dq_open(e, JOURNAL);
        CHECK(dq != NULL);                            /* round: damaged, then cut */
        CHECK(!dq || gptps_dq_pending(dq) == pending_at_start(round));
        CHECK(file_is(g_names[6], g_orig, g_orig_len));   /* .corrupt.4, the oldest */
        for (k = 2; k < NNAME; ++k) if (k != 6) CHECK(file_is(g_names[k], text[k], (long)strlen(text[k])));
        gptps_shutdown(e);
        if (dq) gptps_dq_close(dq);
    }
}

/* A damaged journal no copy of which can be made - here, "<journal>.tmp", which the copy
 * is written to first, cannot be created. Past a cut the records after it would be lost
 * with the compaction, so the open fails and leaves the journal as it was. Confined
 * damage loses nothing more, and the open goes on as it always has. */
static void case_no_copy(void)
{
    int round;
    for (round = R_DAMAGED; round <= R_CUT; ++round) {
        gptps *e = open_engine();
        gptps_dq *dq;
        write_initial(round);
        CHECK(e != NULL); if (!e) return;
        g_warns = 0;
        g_pfail_name = 1; g_pfail_skip = 0;           /* the copy's "<journal>.tmp" */
        dq = gptps_dq_open(e, JOURNAL);
        CHECK(g_pfail_name == -1);                    /* it was made to fail */
        CHECK(!exists(g_names[2]));                   /* no copy */
        CHECK(g_warns == 1);
        if (round == R_CUT) {
            CHECK(dq == NULL);
            CHECK(file_is(JOURNAL, g_orig, g_orig_len));
            CHECK(strstr(g_warn, "the queue is not opened") != NULL);
        } else {
            CHECK(dq != NULL);
            CHECK(!dq || gptps_dq_pending(dq) == pending_at_start(round));
            CHECK(strstr(g_warn, "could not be preserved") != NULL);
        }
        gptps_shutdown(e);
        if (dq) gptps_dq_close(dq);
    }

    /* An open that fails after the copy - here its compaction cannot create its file -
     * leaves the journal as it was, and a retry finds the copy it made: it does not
     * make another, so retries do not use up the slots and the copies of earlier
     * incidents with them. */
    {
        gptps *e = open_engine();
        gptps_dq *dq;
        write_initial(R_CUT);
        CHECK(e != NULL); if (!e) return;
        g_pfail_name = 1; g_pfail_skip = 1;           /* the compaction's, after the copy's */
        dq = gptps_dq_open(e, JOURNAL);
        CHECK(dq == NULL && g_pfail_name == -1);
        CHECK(file_is(JOURNAL, g_orig, g_orig_len));
        CHECK(file_is(g_names[2], g_orig, g_orig_len));
        if (!dq) dq = gptps_dq_open(e, JOURNAL);     /* the retry */
        CHECK(dq != NULL);
        CHECK(!dq || gptps_dq_pending(dq) == pending_at_start(R_CUT));
        CHECK(!exists(g_names[3]));                   /* no second copy */
        gptps_shutdown(e);
        if (dq) gptps_dq_close(dq);
    }
    g_pfail_name = -1;
}

/* A file and a directory this process may not read. Root reads anything, so as root the
 * child runs as uid 65534 first. Each part reports a skip where it cannot set up the
 * condition it is about.
 *   - An earlier ".corrupt" copy this process cannot read (mode 0200) is still a copy:
 *     the new one goes to ".corrupt.1". A slot used to count as free whenever it could
 *     not be opened for reading, and the earlier copy was overwritten.
 *   - In a directory it may write and search but not read (mode 0300), the queue cannot
 *     open the directory to sync it (EACCES). That is a directory that cannot be synced,
 *     not a failed sync: the queue opens, journals and compacts as it always did. */
#define CASE_DIR "dq_crash.d"
static void case_unreadable(void)
{
    static const char ev[] = "an earlier incident";
    static const char jp[] = CASE_DIR "/q.journal", cp[] = CASE_DIR "/q.journal.corrupt",
                      cp1[] = CASE_DIR "/q.journal.corrupt.1", tp[] = CASE_DIR "/q.journal.tmp";
    pid_t pid;
    int wst = 0;
    chmod(CASE_DIR, 0700);
    unlink(jp); unlink(cp); unlink(cp1); unlink(tp);
    rmdir(CASE_DIR);
    CHECK(mkdir(CASE_DIR, 0700) == 0);
    if (geteuid() == 0 && chown(CASE_DIR, 65534, 65534) != 0) { printf("SKIP unreadable files: chown\n"); return; }
    fflush(stdout);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        gptps *e;
        gptps_dq *dq;
        gptps_handle h;
        unsigned char pl[PL_MAX];
        int fd, i;
        alarm(30);
        fails = 0;
        if (geteuid() == 0 && (setgid(65534) != 0 || setuid(65534) != 0)) {
            printf("SKIP unreadable files: cannot run as uid 65534\n"); fflush(stdout); _exit(0);
        }
        if (access(CASE_DIR, W_OK | X_OK) != 0) {
            printf("SKIP unreadable files: %s is out of reach\n", CASE_DIR); fflush(stdout); _exit(0);
        }

        write_journal(jp, R_DAMAGED);
        put_file(cp, ev, time(NULL));
        CHECK(chmod(cp, 0200) == 0);
        if ((fd = open(cp, O_RDONLY)) >= 0) {
            close(fd);
            printf("SKIP an unreadable copy: this process reads a mode-0200 file\n");
        } else {
            e = open_engine(); CHECK(e != NULL);
            dq = e ? gptps_dq_open(e, jp) : NULL;
            CHECK(dq != NULL);
            if (e) gptps_shutdown(e);
            if (dq) gptps_dq_close(dq);
            CHECK(chmod(cp, 0600) == 0);
            CHECK(file_is(cp, ev, (long)strlen(ev)));     /* the earlier copy, untouched */
            CHECK(file_is(cp1, g_orig, g_orig_len));      /* and this one beside it */
        }
        unlink(jp); unlink(cp); unlink(cp1);

        CHECK(chmod(CASE_DIR, 0300) == 0);
        if ((fd = open(CASE_DIR, O_RDONLY | O_DIRECTORY)) >= 0 || errno != EACCES) {
            if (fd >= 0) close(fd);
            printf("SKIP a directory that cannot be read: this process reads a mode-0300 one\n");
        } else {
            e = open_engine(); CHECK(e != NULL);
            dq = e ? gptps_dq_open(e, jp) : NULL;
            CHECK(dq != NULL);
            for (i = 1; dq && i <= 3; ++i)
                CHECK(gptps_dq_submit(dq, kind_name[K_OK], pl, make_payload(i, pl), &h) == GPTPS_OK);
            CHECK(!dq || gptps_dq_compact(dq) == GPTPS_OK);
            if (e) gptps_shutdown(e);
            if (dq) gptps_dq_close(dq);
            e = open_engine(); CHECK(e != NULL);
            dq = e ? gptps_dq_open(e, jp) : NULL;         /* a journal there already */
            CHECK(dq != NULL && gptps_dq_pending(dq) == 3);
            if (e) gptps_shutdown(e);
            if (dq) gptps_dq_close(dq);
        }
        CHECK(chmod(CASE_DIR, 0700) == 0);
        unlink(jp); unlink(tp);
        fflush(stdout);
        _exit(fails ? 1 : 0);
    }
    if (pid < 0) return;
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR) { }
    if (!WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
        printf("FAIL unreadable files: the case's child %s %d\n",
               WIFEXITED(wst) ? "exited with" : "died of signal",
               WIFEXITED(wst) ? WEXITSTATUS(wst) : WTERMSIG(wst));
        ++fails;
    }
    chmod(CASE_DIR, 0700);
    unlink(jp); unlink(cp); unlink(cp1); unlink(tp);
    CHECK(rmdir(CASE_DIR) == 0);
}

int main(int argc, char **argv)
{
    uint32_t seed, first = 1, seeds = 1, total, n;
    uint8_t kinds[NEV];
    int round, group, mode, k, fd;
    struct stat st;
    struct sigaction sa;
    time_t t0 = time(NULL);
    if (argc > 1) seeds = (uint32_t)strtoul(argv[1], NULL, 10);
    if (argc > 2) first = (uint32_t)strtoul(argv[2], NULL, 10);
    if (!seeds) seeds = 1;
    if (getenv("GPTPS_CRASH_SYNC_DELAY_US")) g_sync_delay_us = strtol(getenv("GPTPS_CRASH_SYNC_DELAY_US"), NULL, 10);

    fd = open("/dev/zero", O_RDWR);
    S = fd >= 0 ? (shared *)mmap(NULL, sizeof *S, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : NULL;
    if (fd >= 0) close(fd);
    if (!S || S == (shared *)MAP_FAILED) { printf("FAIL: no shared memory\n"); return 1; }
    snprintf(g_names[0], sizeof g_names[0], "%s", JOURNAL);
    snprintf(g_names[1], sizeof g_names[1], "%s.tmp", JOURNAL);
    snprintf(g_names[2], sizeof g_names[2], "%s.corrupt", JOURNAL);
    for (k = 1; k <= 9; ++k) snprintf(g_names[2 + k], sizeof g_names[2 + k], "%s.corrupt.%d", JOURNAL, k);
    CHECK(stat(".", &st) == 0);
    g_cwd_dev = st.st_dev; g_cwd_ino = st.st_ino;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    gptps_set_log_sink(count_sink, NULL);

    /* GPTPS_CRASH_ONLY=<round>,<threads>,<mode>,<call>[,<einval>] (numbers, as the enums
     * count; threads 0 or 1; einval 1: every fsync of the directory fails with EINVAL)
     * runs one case: to look again at a failure the sweep printed. */
    if (getenv("GPTPS_CRASH_ONLY")) {
        unsigned r = 0, g = 0, m = 0, c = 0, d = 0;
        int got = sscanf(getenv("GPTPS_CRASH_ONLY"), "%u,%u,%u,%u,%u", &r, &g, &m, &c, &d);
        if (got < 4 || r >= R_N || g > 1 || m >= M_N || d > 1) {
            printf("GPTPS_CRASH_ONLY wants <round 0-%d>,<threads 0-1>,<mode 0-%d>,<call>[,<einval 0-1>]\n",
                   R_N - 1, M_N - 1);
            return 2;
        }
        run_one(first, (int)r, (int)g, (int)m, c, (int)d);
        printf("%s: %d failure(s)\n", g_ctx, fails);
        if (g_dumped == 0) dump_left();
        return fails ? 1 : 0;
    }

    for (seed = first; seed < first + seeds; ++seed) {
        for (round = 0; round < R_N; ++round) {
            for (group = 0; group < 2; ++group) {
                /* First the whole workload, uncrashed, to learn its calls. With one
                 * thread they come in the same order every time. Three submitters make
                 * theirs in whatever order they race to, so a later run's Nth call can
                 * differ from this one's - which spreads the sweep over more
                 * interleavings, and is why each run reports the call it hit. */
                run_one(seed, round, group, M_AFTER, 0, 0);
                total = S->events;
                CHECK(total > 50 && total <= NEV);
                if (total > NEV) total = NEV;
                memcpy(kinds, S->ev_io, total);
                for (mode = 0; mode < M_N; ++mode)
                    for (n = 1 + (seed + (uint32_t)round) % SWEEP_STRIDE; n <= total; n += SWEEP_STRIDE)
                        if (applies(mode, kinds[n - 1])) run_one(seed, round, group, mode, n, 0);
                /* A directory that cannot be synced is not an error: the workload, with
                 * every fsync of the directory failing with EINVAL, goes as written. */
                run_one(seed, round, group, M_AFTER, 0, 1);
            }
        }
    }
    snprintf(g_ctx, sizeof g_ctx, "single cases");
    case_copies_full();
    case_no_copy();
    case_unreadable();
    for (k = 0; k < NNAME; ++k) unlink(g_names[k]);

    /* Two errors must have broken the queue somewhere, and made a group commit's sync of
     * the directory fail, or that mode tests less than it says. */
    if (!g_broke_runs) { printf("FAIL: no run broke the queue\n"); ++fails; }
    if (!g_dir2_runs) { printf("FAIL: no second error was a directory's fsync\n"); ++fails; }

    /* The wrappers saw every kind of call they exist for: a --wrap that a libc's
     * redirect bypassed would otherwise make this test pass while crashing nothing. */
    for (k = 0; k < IO_N; ++k)
        if (!g_io_total[k]) { printf("FAIL: no %s call was ever seen\n", io_name[k]); ++fails; }
    for (n = 0, k = 0; k <= M_N; ++k) n += g_runs[k];
    printf("durable_crash: %u seed(s), %s, %u runs:", seeds,
           SWEEP_STRIDE > 1 ? "one call in " STR(SWEEP_STRIDE) " (a sanitized build)" : "every call", n);
    for (k = 0; k < M_N; ++k) printf(" %s %u,", mode_name[k], g_runs[k]);
    printf(" uncrashed %u (%u with the directory failing EINVAL); the queue broke in %u, a second"
           " error hit the directory in %u; %lds\n", g_runs[M_N], g_einval_runs, g_broke_runs,
           g_dir2_runs, (long)(time(NULL) - t0));
    munmap(S, sizeof *S);
    if (fails) { printf("%d durable-crash check(s) FAILED\n", fails); return 1; }
    printf("all durable-crash checks passed\n");
    return 0;
}
