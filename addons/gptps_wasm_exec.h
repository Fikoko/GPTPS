/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * wasm_exec.h - run WebAssembly modules as GPTPS tasks (optional add-on).
 *
 * WHY THIS SHAPE: a .wasm module is portable, sandboxed task code - "write one
 * task, run it on any hardware," exactly GPTPS's aim. But a wasm *interpreter*
 * is a heavy dependency, and bundling one would tie the portable core to a single
 * runtime. So this add-on owns the GPTPS-side integration (module-as-task:
 * admission, cost/priority, retries/timeout, result delivery, and - via OOP -
 * OS-enforced memory caps + hard-kill) and leaves the runtime PLUGGABLE: you
 * supply one function that invokes wasm3 / wasmtime / WAMR / etc.
 *
 * This keeps the core dependency-free and lets any runtime drop in. (The simplest
 * no-glue alternative remains GPTPS_EXEC_PROGRAM with a runtime CLI, e.g.
 * argv = {"wasmtime", "module.wasm", NULL}.)
 *
 * Built only on the public API (no core ABI change); portable (no platform calls
 * of its own). OOP mode uses the fork-based executor (POSIX only), so the runtime
 * must be fork-safe to use it; in-process mode runs everywhere, and is cancellable
 * only through gptps_wasm_install_ex (see there).
 */
#ifndef GPTPS_WASM_EXEC_H
#define GPTPS_WASM_EXEC_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* You implement this against your wasm runtime: instantiate the module at
 * `module_path`, feed it `payload` (len bytes) as input, run its entry point,
 * and return the output as malloc'd bytes via *out_result / *out_len (the engine
 * frees them). Return GPTPS_OK on success, or a gptps_status error. May be called
 * concurrently (one call per task run) and, in OOP mode, from a forked child. */
typedef gptps_status (*gptps_wasm_run_fn)(const char *module_path,
                                          const void *payload, size_t len,
                                          void **out_result, size_t *out_len,
                                          void *user_data);

/* The same, plus the item's `ctx` - the only handle through which a cancel, a timeout
 * or a draining shutdown can reach code running in-process. Poll
 * gptps_is_cancelled(ctx) where your runtime lets you: in a host import the module
 * calls, between fuel/epoch slices (wasmtime epoch interruption, WAMR's
 * wasm_runtime_terminate, wasm3's per-call checks), or from a watchdog thread that
 * then interrupts the instance. Return GPTPS_E_CANCELLED once it is set. `ctx` is
 * valid only for the duration of the call. */
typedef gptps_status (*gptps_wasm_run_ex_fn)(gptps_ctx *ctx, const char *module_path,
                                             const void *payload, size_t len,
                                             void **out_result, size_t *out_len,
                                             void *user_data);

typedef struct gptps_wasm gptps_wasm;

/* Install the WASM executor on engine `e`, backed by runtime callback `run`.
 * Returns NULL on allocation failure or if run == NULL.
 *
 * A `run` callback is given no ctx, so an IN-PROCESS module run through it CANNOT be
 * cancelled: gptps_cancel, its timeout, gptps_unregister_task(GPTPS_REMOVE_CANCEL)
 * and a draining gptps_shutdown all wait until the module returns by itself (see
 * gptps.h on in-process cancel). Use gptps_wasm_install_ex for in-process modules
 * that may run long, or OOP mode (POSIX), where the child is killed. */
gptps_wasm *gptps_wasm_install(gptps *e, gptps_wasm_run_fn run, void *user_data);

/* As gptps_wasm_install, with a runtime callback that receives the item's ctx, so
 * cooperative cancel works in-process too. Returns NULL on allocation failure or if
 * run == NULL. */
gptps_wasm *gptps_wasm_install_ex(gptps *e, gptps_wasm_run_ex_fn run, void *user_data);

/* Register task type `name` that runs the wasm module at `module_path`.
 *   oop != 0 : run in a forked, OS-capped child (sandbox + hard-kill on timeout);
 *              POSIX only - on Windows each run fails with GPTPS_E_INVAL.
 *   oop == 0 : run in-process. Cancel is cooperative: it reaches the module only
 *              through the ctx that gptps_wasm_install_ex's callback is given, and
 *              only when that callback polls it. With gptps_wasm_install's
 *              callback, an in-process module cannot be cancelled at all.
 * `cost` / `policy` may be NULL for defaults. Returns the engine's register
 * status. The module_path string is copied. */
gptps_status gptps_wasm_register(gptps_wasm *w, const char *name, const char *module_path,
                                 int oop, const gptps_cost *cost, const gptps_failure_policy *policy);

/* Free the executor and its per-task bookkeeping. Call AFTER gptps_shutdown(e). */
void gptps_wasm_close(gptps_wasm *w);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_WASM_EXEC_H */
