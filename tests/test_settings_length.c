/* SPDX-License-Identifier: MIT */
/* Reject input loss before engine-owned settings are stored. */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails;
static int notifications;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void watch(const char *key, const char *value, void *ud)
{ (void)key; (void)value; (void)ud; ++notifications; }

typedef struct task_check { const char *expected; int calls; } task_check;
static gptps_status read_task(gptps_ctx *ctx, void *ud)
{
    task_check *c = (task_check *)ud;
    char b[GPTPS_SETTINGS_VALUE_MAX];
    ++c->calls;
    CHECK(gptps_task_setting_str(ctx, "value", b, sizeof b) == GPTPS_OK);
    CHECK(strcmp(b, c->expected) == 0);
    return GPTPS_OK;
}

static gptps *open_manual(void)
{
    gptps *e = NULL;
    gptps_config cfg = {0};
    cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

static void register_reader(gptps *e, task_check *c)
{
    gptps_task_def d = {0};
    d.struct_size = sizeof d; d.name = "reader";
    d.run = read_task; d.user_data = c; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static void check_values(gptps *e, task_check *c, const char *expected)
{
    char b[GPTPS_SETTINGS_VALUE_MAX];
    gptps_handle h;
    size_t ran = 0;
    int previous = c->calls;
    CHECK(gptps_settings_get(e, "app.value", b, sizeof b) == GPTPS_OK);
    CHECK(strcmp(b, expected) == 0);
    CHECK(gptps_settings_get(e, "tasks.reader.value", b, sizeof b) == GPTPS_OK);
    CHECK(strcmp(b, expected) == 0);
    c->expected = expected;
    CHECK(gptps_submit(e, "reader", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_step(e, &ran) == GPTPS_OK);
    CHECK(c->calls == previous + 1);
}

static void test_owned(gptps_setting_type type)
{
    gptps *e = open_manual();
    task_check c = {0};
    char fit[GPTPS_SETTINGS_VALUE_MAX], over[GPTPS_SETTINGS_VALUE_MAX + 1];
    char choices[2 * GPTPS_SETTINGS_VALUE_MAX + 16];
    const char *constraint = NULL;
    size_t n = GPTPS_SETTINGS_VALUE_MAX;
    if (!e) return;
    memset(fit, type == GPTPS_SETTING_ENUM ? 'x' : '0', n - 1);
    fit[n - 2] = type == GPTPS_SETTING_ENUM ? 'x' : '1'; fit[n - 1] = 0;
    memset(over, type == GPTPS_SETTING_ENUM ? 'x' : '0', n);
    over[n - 1] = type == GPTPS_SETTING_ENUM ? 'x' : '1'; over[n] = 0;
    if (type == GPTPS_SETTING_ENUM) {
        snprintf(choices, sizeof choices, "1|%s|%s", fit, over);
        constraint = choices;
    }
    CHECK(gptps_define_global(e, "app.bad", type, over, constraint, 0) == GPTPS_E_CONFIG);
    CHECK(gptps_define_task_setting(e, "bad", type, over, constraint, 0) == GPTPS_E_CONFIG);
    CHECK(gptps_define_global(e, "app.value", type, fit, constraint, 0) == GPTPS_OK);
    CHECK(gptps_define_task_setting(e, "value", type, fit, constraint, 0) == GPTPS_OK);
    register_reader(e, &c);
    check_values(e, &c, fit);
    CHECK(gptps_settings_set(e, "app.value", "1") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.reader.value", "1") == GPTPS_OK);
    CHECK(gptps_settings_watch(e, watch, NULL) == GPTPS_OK);
    notifications = 0;
    CHECK(gptps_settings_set(e, "app.value", over) == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set(e, "tasks.reader.value", over) == GPTPS_E_CONFIG);
    CHECK(notifications == 0);
    check_values(e, &c, "1");
    CHECK(gptps_settings_set(e, "app.value", fit) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.reader.value", fit) == GPTPS_OK);
    check_values(e, &c, fit);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

static void test_reload(gptps_setting_type type)
{
    gptps *e = open_manual();
    task_check c = {0};
    char value[GPTPS_SETTINGS_VALUE_MAX + 2];
    char fit[GPTPS_SETTINGS_VALUE_MAX];
    char choices[GPTPS_SETTINGS_VALUE_MAX + 16];
    const char *constraint = NULL;
    const size_t lengths[] = {0, GPTPS_SETTINGS_VALUE_MAX - 1,
                             GPTPS_SETTINGS_VALUE_MAX, GPTPS_SETTINGS_VALUE_MAX + 1};
    size_t i;
    if (!e) return;
    memset(fit, 'x', sizeof fit - 1); fit[sizeof fit - 1] = 0;
    if (type == GPTPS_SETTING_ENUM) {
        snprintf(choices, sizeof choices, "original|%s", fit);
        constraint = choices;
    }
    CHECK(gptps_define_global(e, "app.value", type, "original", constraint, 0) == GPTPS_OK);
    CHECK(gptps_define_task_setting(e, "value", type, "original", constraint, 0) == GPTPS_OK);
    register_reader(e, &c);
    for (i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        FILE *f;
        gptps_status expected = lengths[i] < GPTPS_SETTINGS_VALUE_MAX ? GPTPS_OK : GPTPS_E_CONFIG;
        if (type == GPTPS_SETTING_ENUM && lengths[i] == 0) continue;
        memset(value, 'x', lengths[i]); value[lengths[i]] = 0;
        CHECK(gptps_settings_set(e, "app.value", "original") == GPTPS_OK);
        CHECK(gptps_settings_set(e, "tasks.reader.value", "original") == GPTPS_OK);
        CHECK(gptps_settings_set(e, "app.value", value) == expected);
        CHECK(gptps_settings_set(e, "tasks.reader.value", value) == expected);
        check_values(e, &c, expected == GPTPS_OK ? value : "original");
        CHECK(gptps_settings_set(e, "app.value", "original") == GPTPS_OK);
        CHECK(gptps_settings_set(e, "tasks.reader.value", "original") == GPTPS_OK);
        f = fopen("settings_length.toml", "wb");
        CHECK(f != NULL);
        if (!f) continue;
        fprintf(f, "[app]\nvalue = \"%s\"\n[tasks.reader]\nvalue = \"%s\"\n", value, value);
        CHECK(fclose(f) == 0);
        CHECK(gptps_settings_reload(e, "settings_length.toml") == expected);
        check_values(e, &c, expected == GPTPS_OK ? value : "original");
        CHECK(remove("settings_length.toml") == 0);
    }
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "owned") != 0 && strcmp(argv[1], "reload") != 0))
        return 2;
    if (argc == 1 || strcmp(argv[1], "owned") == 0) {
        test_owned(GPTPS_SETTING_INT);
        test_owned(GPTPS_SETTING_UINT);
        test_owned(GPTPS_SETTING_DOUBLE);
        test_owned(GPTPS_SETTING_ENUM);
    }
    if (argc == 1 || strcmp(argv[1], "reload") == 0) {
        test_reload(GPTPS_SETTING_STRING);
        test_reload(GPTPS_SETTING_ENUM);
    }
    printf("settings length failures: %d\n", fails);
    return fails ? 1 : 0;
}
