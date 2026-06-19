/*
 * wasm_program.c - run a WebAssembly module as a GPTPS task, with NO new code in
 * the library: a wasm runtime CLI is just an external program, so a .wasm module
 * runs through GPTPS_EXEC_PROGRAM. argv[0] = "wasmtime" is PATH-resolved; the
 * engine pipes the task payload to the module's stdin and captures its stdout as
 * the result, under the same budget / timeout / retry machinery as any task.
 *
 * This embeds a tiny valid module (an empty WASI `_start`, so it just runs and
 * exits 0) to keep the example self-contained. A real module would read stdin and
 * write stdout (compile your task to wasm32-wasi); nothing here changes.
 *
 * Install a runtime to run it for real:  https://wasmtime.dev  (or `wasmer`).
 * Without one on PATH, the example prints a note and skips.
 *
 *   cc wasm_program.c gptps.c -lpthread -ldl   (amalgamation)
 */
#include "gptps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal WebAssembly module: (module (func $s) (export "_start" (func $s)))
 * Hand-assembled per the binary spec - exports an empty `_start`, imports
 * nothing, so `wasmtime run` executes it and exits 0. */
static const unsigned char kWasmModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, /* magic + version */
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,             /* type:   () -> () */
    0x03, 0x02, 0x01, 0x00,                         /* func:   one, type 0 */
    0x07, 0x0a, 0x01, 0x06, 0x5f, 0x73, 0x74, 0x61, 0x72, 0x74, 0x00, 0x00, /* export "_start" func 0 */
    0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b              /* code:   empty body (end) */
};

#define MODULE_PATH "gptps_demo_module.wasm"

static int g_finished = 0;
static void on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) g_finished = 1;
    else if (ev->kind == GPTPS_EV_FAILED)
        printf("  wasm task failed: %s\n", gptps_strerror(ev->status));
}

int main(void)
{
    static const char *argv[] = { "wasmtime", "run", MODULE_PATH, (const char *)0 };
    gptps *engine;
    gptps_task_def def;
    gptps_handle h;
    FILE *f;

    /* skip cleanly if no runtime is installed */
    if (system("wasmtime --version >/dev/null 2>&1") != 0) {
        printf("wasmtime not found on PATH - skipping.\n"
               "  install it (https://wasmtime.dev) and re-run to execute the module.\n");
        return 0;
    }

    f = fopen(MODULE_PATH, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", MODULE_PATH); return 1; }
    fwrite(kWasmModule, 1, sizeof kWasmModule, f);
    fclose(f);

    if (gptps_open(NULL, &engine) != GPTPS_OK) { fprintf(stderr, "open failed\n"); remove(MODULE_PATH); return 1; }
    gptps_set_event_cb(engine, on_event, NULL);

    memset(&def, 0, sizeof def);
    def.struct_size = sizeof def;
    def.name = "wasm";
    def.exec = GPTPS_EXEC_PROGRAM;     /* a wasm runtime is just an external program */
    def.argv = argv;
    def.default_cost.struct_size = sizeof def.default_cost;
    def.default_policy.struct_size = sizeof def.default_policy;
    def.default_policy.timeout_seconds = 10;
    gptps_register_task(engine, &def);

    printf("GPTPS wasm demo: running %s via wasmtime as a task...\n", MODULE_PATH);
    gptps_submit(engine, "wasm", NULL, 0, &h);

    gptps_shutdown(engine);
    remove(MODULE_PATH);
    printf(g_finished ? "wasm module ran and finished.\n" : "wasm module did not finish.\n");
    return g_finished ? 0 : 1;
}
