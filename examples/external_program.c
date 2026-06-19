/*
 * external_program.c - run an EXTERNAL PROGRAM as a GPTPS task.
 *
 * GPTPS_EXEC_PROGRAM forks + execs a binary you name in def.argv: the task's
 * payload is fed to the program's stdin, and its stdout comes back as the task
 * result (delivered on the FINISHED event). The program is OS-capped and
 * hard-killed on timeout - so "any language, any binary" becomes a task with the
 * same budget + failure policy as in-process work. Here we use /bin/cat, which
 * echoes stdin to stdout, so the result equals the payload.
 *
 *   cc external_program.c gptps.c -lpthread -ldl   (amalgamation)
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static const char *kInput = "hello from gptps\n";
static int g_ok = 0;

static void on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) {
        printf("  result (%zu bytes): %.*s", ev->result_len, (int)ev->result_len,
               ev->result ? (const char *)ev->result : "");
        g_ok = (ev->result_len == strlen(kInput) &&
                memcmp(ev->result, kInput, ev->result_len) == 0);
    } else if (ev->kind == GPTPS_EV_FAILED) {
        printf("  program task failed: %s\n", gptps_strerror(ev->status));
    }
}

int main(void)
{
    /* argv for the child: NULL-terminated, argv[0] is the binary to exec */
    static const char *cat_argv[] = { "/bin/cat", NULL };
    gptps *engine;
    gptps_task_def def;
    gptps_handle h;

    if (gptps_open(NULL, &engine) != GPTPS_OK) { fprintf(stderr, "open failed\n"); return 1; }
    gptps_set_event_cb(engine, on_event, NULL);

    memset(&def, 0, sizeof def);
    def.struct_size = sizeof def;
    def.name = "echo";
    def.exec = GPTPS_EXEC_PROGRAM;        /* run a program, not a C function */
    def.argv = cat_argv;                  /* payload -> its stdin, stdout -> result */
    def.default_cost.struct_size = sizeof def.default_cost;
    def.default_cost.mem_bytes = 8u << 20;
    def.default_policy.struct_size = sizeof def.default_policy;
    def.default_policy.timeout_seconds = 5; /* hard-killed if it overruns */
    if (gptps_register_task(engine, &def) != GPTPS_OK) { fprintf(stderr, "register failed\n"); return 1; }

    printf("GPTPS external-program demo: piping %zu bytes through /bin/cat...\n", strlen(kInput));
    gptps_submit(engine, "echo", kInput, strlen(kInput), &h);

    gptps_shutdown(engine);
    printf(g_ok ? "round-trip OK.\n" : "round-trip MISMATCH.\n");
    return g_ok ? 0 : 1;
}
