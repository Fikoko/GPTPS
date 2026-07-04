/*
 * test_pool.c - the N-shard router (scale-up by composition). Proves work spreads
 * evenly round-robin across independent engine shards, that keyed routing pins a
 * key to a fixed shard (affinity), that a pool handle cancels on the right shard,
 * and that dead-letters aggregate across shards. Built on the public API only.
 */
#include "gptps_pool.h"
#include <stdio.h>
#include <string.h>

#define NSHARD 4

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int fin[NSHARD], started;
static int  idx_arr[NSHARD] = { 0, 1, 2, 3 };
static int  inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int  get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void reset(void) { int i; for (i = 0; i < NSHARD; ++i) __atomic_store_n(&fin[i], 0, __ATOMIC_SEQ_CST); __atomic_store_n(&started, 0, __ATOMIC_SEQ_CST); }
static int  total_fin(void) { int i, s = 0; for (i = 0; i < NSHARD; ++i) s += get(&fin[i]); return s; }

static void obs(const gptps_event *ev, void *ud)
{
    int shard = *(int *)ud;
    if (ev->kind == GPTPS_EV_FINISHED) inc(&fin[shard]);
    if (ev->kind == GPTPS_EV_STARTED)  inc(&started);
}

static gptps_status task_quick(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static gptps_status task_block(gptps_ctx *c, void *u) { (void)u; while (!gptps_is_cancelled(c)) { } return GPTPS_OK; }
static gptps_status task_fail(gptps_ctx *c, void *u)  { (void)c; (void)u; return GPTPS_E_TASK; }

/* open a pool, register `name`->fn on every shard, and a per-shard completion observer */
static gptps_pool *make_pool(gptps_run_fn fn, const char *name)
{
    gptps_config cfg;
    gptps_pool *p;
    gptps_task_def d;
    size_t i;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2;     /* per shard; aggregate = NSHARD*2 */
    p = gptps_pool_open(NSHARD, &cfg);
    if (!p) return NULL;

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;   /* on_failure=DEAD_LETTER, max_retries=0 */
    CHECK(gptps_pool_register_task(p, &d) == GPTPS_OK);

    for (i = 0; i < NSHARD; ++i)
        gptps_register_observer(gptps_pool_shard(p, i), obs, &idx_arr[i]);
    return p;
}

static int wait_total(int target, unsigned ms)
{ uint64_t s = gptps_now_ms(NULL); while (total_fin() < target && gptps_now_ms(NULL) - s < ms) { } return total_fin() >= target; }
static int wait_started(unsigned ms)
{ uint64_t s = gptps_now_ms(NULL); while (get(&started) < 1 && gptps_now_ms(NULL) - s < ms) { } return get(&started) >= 1; }

/* 1) round-robin spreads load evenly across shards */
static void test_round_robin(void)
{
    gptps_pool *p;
    int k;
    reset();
    p = make_pool(task_quick, "q"); CHECK(p); if (!p) return;
    CHECK(gptps_pool_count(p) == NSHARD);
    for (k = 0; k < 40; ++k) CHECK(gptps_pool_submit(p, "q", NULL, 0, NULL) == GPTPS_OK);
    CHECK(wait_total(40, 3000));
    for (k = 0; k < NSHARD; ++k) CHECK(get(&fin[k]) == 10);   /* 40 / 4 shards, exactly even */
    gptps_pool_close(p);
}

/* 2) keyed routing pins a key to one shard (affinity); the pool handle encodes it */
static void test_affinity(void)
{
    gptps_pool *p;
    gptps_pool_handle h = 0;
    int k;
    reset();
    p = make_pool(task_quick, "q"); CHECK(p); if (!p) return;

    for (k = 0; k < 20; ++k) CHECK(gptps_pool_submit_keyed(p, 7, "q", NULL, 0, &h) == GPTPS_OK);
    CHECK((size_t)(h >> 48) == (size_t)(7 % NSHARD));        /* handle carries the shard: 7%4 = 3 */
    CHECK(wait_total(20, 3000));
    CHECK(get(&fin[7 % NSHARD]) == 20);                      /* every key-7 item on one shard */
    CHECK(get(&fin[0]) == 0 && get(&fin[1]) == 0 && get(&fin[2]) == 0);

    /* distinct keys land on distinct shards */
    CHECK(gptps_pool_submit_keyed(p, 0, "q", NULL, 0, NULL) == GPTPS_OK);  /* -> shard 0 */
    CHECK(gptps_pool_submit_keyed(p, 5, "q", NULL, 0, NULL) == GPTPS_OK);  /* 5%4 -> shard 1 */
    CHECK(wait_total(22, 3000));
    CHECK(get(&fin[0]) == 1 && get(&fin[1]) == 1);
    gptps_pool_close(p);
}

/* 3) a pool handle cancels on the owning shard (clean close proves it) */
static void test_pool_cancel(void)
{
    gptps_pool *p;
    gptps_pool_handle h = 0;
    reset();
    p = make_pool(task_block, "b"); CHECK(p); if (!p) return;

    CHECK(gptps_pool_submit(p, "b", NULL, 0, &h) == GPTPS_OK);
    CHECK(h != 0);
    CHECK(wait_started(3000));
    CHECK(gptps_pool_cancel(p, 0) == GPTPS_E_INVAL);         /* malformed handle */
    CHECK(gptps_pool_cancel(p, h) == GPTPS_OK);              /* routes to the right shard */
    gptps_pool_close(p);                                     /* returns => the spinner was stopped */
}

/* 4) dead-letters aggregate across shards */
static void test_dead_letter_aggregate(void)
{
    gptps_pool *p;
    uint64_t s;
    reset();
    p = make_pool(task_fail, "f"); CHECK(p); if (!p) return;

    CHECK(gptps_pool_submit_keyed(p, 0, "f", NULL, 0, NULL) == GPTPS_OK);   /* shard 0 */
    CHECK(gptps_pool_submit_keyed(p, 1, "f", NULL, 0, NULL) == GPTPS_OK);   /* shard 1 */
    CHECK(gptps_pool_submit_keyed(p, 2, "f", NULL, 0, NULL) == GPTPS_OK);   /* shard 2 */
    s = gptps_now_ms(NULL);
    while (gptps_pool_dead_letter_count(p) < 3 && gptps_now_ms(NULL) - s < 3000) { }
    CHECK(gptps_pool_dead_letter_count(p) == 3);             /* summed across shards */
    gptps_pool_close(p);
}

int main(void)
{
    test_round_robin();
    test_affinity();
    test_pool_cancel();
    test_dead_letter_aggregate();

    if (fails) { printf("%d pool check(s) FAILED\n", fails); return 1; }
    printf("all pool checks passed\n");
    return 0;
}
