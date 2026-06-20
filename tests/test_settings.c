/*
 * test_settings.c - the unified settings registry (Phase 1): registration,
 * introspection, typed get/set with validation, and hot-apply (observed by
 * reading the live value back). Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static gptps_status noop(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static void reg(gptps *e, const char *name)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static int has_key(gptps *e, const char *key)
{
    size_t i, n = gptps_settings_count(e);
    for (i = 0; i < n; ++i) {
        gptps_setting_info info; memset(&info, 0, sizeof info); info.struct_size = sizeof info;
        if (gptps_settings_get_info(e, i, &info) == GPTPS_OK && strcmp(info.key, key) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    gptps *e = NULL;
    char buf[GPTPS_SETTINGS_VALUE_MAX];
    size_t i, n;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return 1;

    /* core settings present before any task */
    CHECK(gptps_settings_count(e) == 3);
    CHECK(has_key(e, "limits.max_memory_bytes"));
    CHECK(has_key(e, "limits.max_concurrent_tasks"));
    CHECK(has_key(e, "scheduler.reserve_after_skips"));

    /* a task adds its six knobs */
    reg(e, "work");
    CHECK(gptps_settings_count(e) == 9);
    CHECK(has_key(e, "tasks.work.timeout_seconds"));
    CHECK(has_key(e, "tasks.work.on_failure"));
    reg(e, "bulk");
    CHECK(gptps_settings_count(e) == 15);

    /* get a default */
    CHECK(gptps_settings_get(e, "scheduler.reserve_after_skips", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "8") == 0);

    /* set + hot-apply (read back the live value) */
    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "3") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "scheduler.reserve_after_skips", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "3") == 0);

    /* per-task set + read back */
    CHECK(gptps_settings_set(e, "tasks.work.priority", "5") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "tasks.work.priority", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "5") == 0);
    CHECK(gptps_settings_set(e, "tasks.work.on_failure", "drop") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "tasks.work.on_failure", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "drop") == 0);

    /* validation: bad enum / bad type / out-of-range / unknown key are rejected,
     * and a rejected set leaves the value unchanged */
    CHECK(gptps_settings_set(e, "tasks.work.on_failure", "bogus") == GPTPS_E_CONFIG);
    CHECK(gptps_settings_get(e, "tasks.work.on_failure", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "drop") == 0);                                  /* unchanged */
    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "abc") == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set(e, "limits.max_concurrent_tasks", "0") == GPTPS_E_CONFIG); /* min 1 */
    CHECK(gptps_settings_set(e, "nope.nope", "1") == GPTPS_E_NOTFOUND);
    CHECK(gptps_settings_get(e, "nope.nope", buf, sizeof buf) == GPTPS_E_NOTFOUND);

    /* introspection metadata: hot flags + value/defval populated */
    n = gptps_settings_count(e);
    for (i = 0; i < n; ++i) {
        gptps_setting_info info; memset(&info, 0, sizeof info); info.struct_size = sizeof info;
        CHECK(gptps_settings_get_info(e, i, &info) == GPTPS_OK);
        CHECK(info.key && info.key[0]);
        if (strcmp(info.key, "scheduler.reserve_after_skips") == 0) {
            CHECK(info.hot == 1);
            CHECK(strcmp(info.value, "3") == 0);     /* current */
            CHECK(strcmp(info.defval, "8") == 0);    /* default snapshotted at registration */
        }
        if (strcmp(info.key, "limits.max_concurrent_tasks") == 0)
            CHECK(info.hot == 0);                    /* restart-only */
    }

    gptps_shutdown(e);
    if (fails) { printf("%d settings check(s) FAILED\n", fails); return 1; }
    printf("all settings checks passed\n");
    return 0;
}
