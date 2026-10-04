/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * fuzz_config.c - a config file end to end, under a fuzzer: everything an
 * operator's file goes through after it is written.
 *
 * The input is written to a file, which gptps_open_ex opens as its config_path. Then
 * the rest of the file's life: a host registers tasks (named after the file's own
 * [tasks.*] tables too) and defines settings, which claim the keys that waited for
 * them; gptps_config_check reports what nothing claimed; a reload reads the file
 * again; a save edits it in place, and a save to a new path copies it. The same file
 * is then read into an engine opened without it, by reload and save. Held to:
 *   - an open that fails says GPTPS_E_CONFIG, and a file that does not parse never
 *     opens an engine;
 *   - a file the engine opened is saved in place, and still parses after;
 *   - a file that does not parse is never touched by a save;
 *   - a save to a new path - a copy of the loaded file, or the settings alone when
 *     that file no longer parses - writes no value the engine refused: an engine set
 *     up the same way reloads it cleanly;
 * and, as everywhere, no crash, no leak, no overrun.
 *
 * A file that names add-ons is not opened: the loader would dlopen whatever path
 * the fuzzer wrote. The rest of it still runs, through reload, which only checks
 * that key's shape.
 */
#include "gptps.h"
#include "gptps_internal.h"   /* the parser, to look at the file before it is opened */
#include "fuzz_common.h"

/* The fuzzer's own settings: a global of each shape a file can set, and a
 * per-task one, defined after open so the keys for them wait to be claimed. */
static void define_settings(gptps *e)
{
    (void)gptps_define_global(e, "app.name", GPTPS_SETTING_STRING, "x", NULL, 0);
    (void)gptps_define_global(e, "app.level", GPTPS_SETTING_INT, "1", "-5..9", 0);
    (void)gptps_define_global(e, "app.ratio", GPTPS_SETTING_DOUBLE, "0.5", "0..1", 0);
    (void)gptps_define_global(e, "app.on", GPTPS_SETTING_BOOL, "false", NULL, 0);
    (void)gptps_define_global(e, "app.mode", GPTPS_SETTING_ENUM, "fast", "fast|slow", 0);
    (void)gptps_define_global(e, "top", GPTPS_SETTING_UINT, "0", NULL, 0);
    (void)gptps_define_task_setting(e, "weight", GPTPS_SETTING_UINT, "1", "0..100", 0);
}

/* The odd names always; the file's own task names for half the inputs, so the other
 * half leave [tasks.*] tables nothing claims, for gptps_config_check to report. */
static uint32_t g_h;

static void register_tasks(gptps *e, const gptps_toml *t)
{
    char names[FZ_MAX_FILE_TASKS][GPTPS_TASK_NAME_MAX + 1];
    size_t i, n = (t && (g_h & 1u)) ? fz_file_task_names(t, names, FZ_MAX_FILE_TASKS) : 0;
    for (i = 0; i < FZ_N_ODD_TASKS; ++i) (void)fz_register(e, FZ_ODD_TASKS[i]);
    for (i = 0; i < n; ++i) (void)fz_register(e, names[i]);
}

static void set_live(gptps *e)
{
    (void)gptps_settings_set(e, "scheduler.reserve_after_skips", "3");
    (void)gptps_settings_set(e, "limits.max_intake_depth", "17");
    (void)gptps_settings_set(e, "tasks.fz.priority", "-2");
    (void)gptps_settings_set(e, "tasks.sp ace.on_failure", "drop");
    (void)gptps_settings_set(e, "app.name", "a \"quoted\" # value\twith\\escapes\n");
    (void)gptps_settings_set(e, "top", "5");
}

static int file_parses(const char *path)
{
    gptps_toml *t = gptps_toml_parse_file(path, NULL, 0);
    gptps_toml_free(t);
    return t != NULL;
}

static void cleanup(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(path);
    remove(tmp);
}

/* What a save wrote to a new path holds no value the engine refused: an engine set
 * up as the input's are - its tasks registered (`t` is the input, which names them),
 * its settings defined - reloads it without a refusal. A refused value copied as it
 * was would be refused again there. */
static void reloads_clean(const gptps_toml *t, const char *copy)
{
    gptps_config cfg;
    gptps *e2 = NULL;
    gptps_status st;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e2) != GPTPS_OK) return;
    register_tasks(e2, t);
    define_settings(e2);
    st = gptps_settings_reload(e2, copy);
    if (st != GPTPS_OK) {
        char *text = fz_slurp(copy, NULL);
        fprintf(stderr, "the saved file:\n---\n%s---\n", text ? text : "");
        free(text);
    }
    FZ_ASSERT(st == GPTPS_OK, "a save to a new path wrote a value the engine refuses");
    gptps_shutdown(e2);
}

/* The file the engine loaded - at open, or by the last reload, which installs its file
 * even when it refuses a value in it - gone bad: a save to a new path writes the
 * values set live and the ones that file set, and what it writes parses and holds no
 * value the engine refuses. A max_memory_gb there goes in as max_memory_bytes: the
 * value the engine runs with, or - when the engine took the file's - that many GiB,
 * which keeps an "auto" 0 as it is. The harness converts only a number it can (0 to
 * 1e9 GiB): 1e20 GiB is past any 64-bit count, a conversion C leaves undefined. */
static void save_without_base(gptps *e, const gptps_toml *input, const gptps_toml *loaded, const char *path,
                              const char *copy)
{
    gptps_toml *t;
    long g, b;
    gptps_status st;
    if (!fz_write(path, "[", 1)) return;
    remove(copy);
    st = gptps_settings_save(e, copy);
    FZ_ASSERT(st == GPTPS_OK, "a save to a new path fails when the loaded file no longer parses");
    t = gptps_toml_parse_file(copy, NULL, 0);
    FZ_ASSERT(t != NULL, "a save without a copy of the loaded file wrote a file that does not parse");
    g = gptps_toml_find_dotted(loaded, "limits.max_memory_gb");
    b = gptps_toml_find_dotted(loaded, "limits.max_memory_bytes");
    if (g >= 0 && b < 0) {
        const char *x = gptps_toml_text_at(loaded, (size_t)g), *y;
        gptps_toml_kind k = gptps_toml_kind_at(loaded, (size_t)g);
        double gb = x ? gptps_strtod_c(x, NULL) : -1;
        long nb = gptps_toml_find_dotted(t, "limits.max_memory_bytes");
        char live[GPTPS_SETTINGS_VALUE_MAX];
        int as_file;
        live[0] = 0;
        (void)gptps_settings_get(e, "limits.max_memory_bytes", live, sizeof live);
        y = nb >= 0 ? gptps_toml_text_at(t, (size_t)nb) : NULL;
        as_file = y && x && (k == GPTPS_TOML_INT || k == GPTPS_TOML_FLOAT) && gb >= 0 && gb <= 1e9 &&
                  strtoull(y, NULL, 10) == (unsigned long long)(gb * 1073741824.0);
        if (!(y && (!strcmp(y, live) || as_file)))
            fprintf(stderr, "max_memory_gb = %s; the engine runs %s; saved max_memory_bytes = %s\n",
                    x ? x : "(a list)", live, y ? y : "(none)");
        FZ_ASSERT(y && (!strcmp(y, live) || as_file),
                  "max_memory_gb is saved as neither the engine's max_memory_bytes nor that many GiB");
    }
    gptps_toml_free(t);
    reloads_clean(input, copy);
    cleanup(copy);
}

/* An engine that opened the file: the rest of the file's life. */
static void life(gptps *e, const gptps_toml *t, const char *path, const char *copy)
{
    gptps_status st;
    register_tasks(e, t);
    define_settings(e);
    st = gptps_config_check(e);
    FZ_ASSERT(st == GPTPS_OK || st == GPTPS_E_CONFIG, "gptps_config_check says something else");
    set_live(e);
    st = gptps_settings_reload(e, NULL);
    FZ_ASSERT(st == GPTPS_OK || st == GPTPS_E_CONFIG, "a reload of the opened file says something else");
    set_live(e);
    st = gptps_settings_save(e, NULL);
    FZ_ASSERT(st == GPTPS_OK, "a file the engine opened is not saved in place");
    FZ_ASSERT(file_parses(path), "a file the engine opened does not parse after a save");
    st = gptps_settings_reload(e, NULL);
    FZ_ASSERT(st == GPTPS_OK || st == GPTPS_E_CONFIG, "a reload of the saved file says something else");
    /* a new path: a copy of the file the engine loaded, with the live values in it */
    remove(copy);
    st = gptps_settings_save(e, copy);
    FZ_ASSERT(st == GPTPS_OK, "a save to a new path fails");
    FZ_ASSERT(file_parses(copy), "a save to a new path wrote a file that does not parse");
    reloads_clean(t, copy);
    (void)gptps_config_check(e);
    cleanup(copy);
    {   /* the reload above installed the saved file */
        gptps_toml *loaded = gptps_toml_parse_file(path, NULL, 0);
        if (loaded) save_without_base(e, t, loaded, path, copy);
        gptps_toml_free(loaded);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int quiet = 0;
    char path[512], copy[512], *was;
    gptps_toml *t;
    gptps_config cfg;
    gptps *e = NULL;
    gptps_status st;
    size_t was_len = 0;
    int parsed, addons = 0;
    if (size > FZ_MAX_INPUT) return 0;
    if (!quiet) { gptps_set_log_sink(fz_quiet_sink, NULL); quiet = 1; }
    fz_path(path, sizeof path, "config.toml");
    fz_path(copy, sizeof copy, "config_copy.toml");
    g_h = fz_hash(data, size);
    if (!fz_write(path, data, size)) return 0;

    /* what the file says, as the engine's parser reads it from the disk */
    t = gptps_toml_parse_file(path, NULL, 0);
    parsed = t != NULL;
    if (t) {
        long j = gptps_toml_find_dotted(t, "addons");
        const char *const *arr;
        addons = j >= 0 && gptps_toml_str_array(t, "", "addons", &arr) > 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;              /* no threads: one input, one deterministic path */
    if (!addons) {
        cfg.config_path = path;
        st = gptps_open_ex(&cfg, &e);
        FZ_ASSERT(st == GPTPS_OK || st == GPTPS_E_CONFIG, "an open with a config file says something else");
        FZ_ASSERT(st != GPTPS_OK || parsed, "a file that does not parse opened an engine");
        FZ_ASSERT((st == GPTPS_OK) == (e != NULL), "an open's status and its engine disagree");
        if (e) {
            life(e, t, path, copy);
            gptps_shutdown(e);
            e = NULL;
            if (!fz_write(path, data, size)) { gptps_toml_free(t); cleanup(path); return 0; }
        }
    }

    /* the same file, read into an engine opened without it */
    cfg.config_path = NULL;
    if (gptps_open_ex(&cfg, &e) == GPTPS_OK) {
        register_tasks(e, t);
        define_settings(e);
        set_live(e);
        st = gptps_settings_reload(e, path);
        FZ_ASSERT(parsed || st == GPTPS_E_CONFIG, "a reload of a file that does not parse is not refused");
        set_live(e);
        was = fz_slurp(path, &was_len);
        st = gptps_settings_save(e, path);
        if (parsed) {
            FZ_ASSERT(st == GPTPS_OK, "a file that parses is not saved in place");
            FZ_ASSERT(file_parses(path), "a file that parsed does not parse after a save");
        } else if (was) {
            size_t now_len = 0;
            char *now = fz_slurp(path, &now_len);
            FZ_ASSERT(st == GPTPS_E_CONFIG || st == GPTPS_E_IO, "a save into a file that does not parse is not refused");
            FZ_ASSERT(now && now_len == was_len && !memcmp(now, was, was_len),
                      "a save changed a file that does not parse");
            free(now);
        }
        free(was);
        if (parsed) {                       /* the reload installed the input, refusals and all */
            remove(copy);
            st = gptps_settings_save(e, copy);
            FZ_ASSERT(st == GPTPS_OK, "a save to a new path fails");
            reloads_clean(t, copy);
            cleanup(copy);
            save_without_base(e, t, t, path, copy);
        }
        gptps_shutdown(e);
    }
    gptps_toml_free(t);
    cleanup(path);
    return 0;
}
