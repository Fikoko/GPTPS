/*
 * task_control.c - the runtime CONTROL PLANE end to end: declare generic
 * settings, enumerate the registry, read a per-task setting from inside a task,
 * clone + pause + unregister task types live - all without recompiling.
 *
 *   cc task_control.c gptps.c -lpthread -ldl   (amalgamation; macOS: drop -ldl)
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

/* a task that reads its own per-task "quality" setting and returns it */
static gptps_status resize(gptps_ctx *ctx, void *ud)
{
    long q = 0; (void)ud;
    gptps_task_setting_int(ctx, "quality", &q);
    return gptps_result_set(ctx, &q, sizeof q);
}

static long last_quality = -1;
static void on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED && ev->result && ev->result_len == sizeof(long))
        memcpy(&last_quality, ev->result, sizeof last_quality);
}

static void reg(gptps *e, const char *name)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = resize; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    gptps_register_task(e, &d);
}

int main(void)
{
    gptps *e;
    gptps_handle h;
    size_t i, n;
    char v[GPTPS_SETTINGS_VALUE_MAX];

    if (gptps_open(NULL, &e) != GPTPS_OK) { fprintf(stderr, "open failed\n"); return 1; }
    gptps_set_event_cb(e, on_event, NULL);

    /* 1) declare generic settings at runtime - no per-key get/set glue */
    gptps_define_global(e, "app.max_upload_mb", GPTPS_SETTING_UINT, "10", "0..4096", 0);
    gptps_define_task_setting(e, "quality", GPTPS_SETTING_UINT, "75", "0..100", 0);

    /* 2) register tasks; each automatically gets a tasks.<name>.quality knob */
    reg(e, "resize");
    reg(e, "thumbnail");

    /* 3) enumerate the registry (what a control UI renders from) */
    n = gptps_task_count(e);
    printf("registered %lu task type(s):\n", (unsigned long)n);
    for (i = 0; i < n; ++i) {
        gptps_task_info ti; memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
        if (gptps_task_get_info(e, i, &ti) == GPTPS_OK)
            printf("  - %-12s exec=%d prio=%d enabled=%d\n", ti.name, (int)ti.exec, ti.priority, ti.enabled);
    }

    /* 4) tune one task's per-instance setting, submit, and read it back inside run() */
    gptps_settings_set(e, "tasks.resize.quality", "92");
    gptps_settings_get(e, "tasks.resize.quality", v, sizeof v);
    printf("tasks.resize.quality = %s\n", v);
    gptps_submit(e, "resize", NULL, 0, &h);
    { uint64_t s = gptps_now_ms(NULL);
      while (__atomic_load_n(&last_quality, __ATOMIC_SEQ_CST) < 0 && gptps_now_ms(NULL) - s < 2000) {} }
    printf("resize run() read quality = %ld\n", last_quality);

    /* 5) clone, retune the copy, pause + resume, then remove a type cleanly */
    gptps_clone_task(e, "resize", "resize_hi");
    gptps_settings_set(e, "tasks.resize_hi.quality", "100");
    gptps_set_task_enabled(e, "thumbnail", 0);
    printf("thumbnail available after pause? %d\n", gptps_task_exists(e, "thumbnail"));
    gptps_set_task_enabled(e, "thumbnail", 1);

    gptps_unregister_task(e, "resize_hi", GPTPS_REMOVE_DRAIN);   /* drain in-flight, then free */
    printf("after delete, resize_hi exists? %d\n", gptps_task_exists(e, "resize_hi"));
    printf("task types now: %lu\n", (unsigned long)gptps_task_count(e));

    gptps_shutdown(e);
    printf("task control demo done.\n");
    return 0;
}
