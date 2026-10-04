/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_internal.h - internal prototypes shared across core translation units.
 * Not installed; not part of the public ABI.
 */
#ifndef GPTPS_INTERNAL_H
#define GPTPS_INTERNAL_H

#include "gptps.h"
#include "gptps_hal.h"   /* the acquire/release pair (executor cancel), gptps_hal_monotonic_ms */

#include <stddef.h>   /* offsetof */

/* --- append-safe ABI struct guards ---
 * A caller-extensible struct only ever GROWS by appending (see gptps.h "ABI
 * DISCIPLINE"), so validating an INPUT struct with `struct_size < sizeof(current)`
 * would reject a caller compiled against an OLDER header the moment the core adds a
 * field. Instead validate against a FROZEN minimum (the layout at the point the
 * field became required) and read any later-appended field only when struct_size
 * proves the caller actually has it.
 *
 *   GPTPS_STRUCT_HAS(type, ptr, field): true iff `ptr` is large enough to contain
 *   `field` (guard before reading an appended field).
 *   GPTPS_<STRUCT>_MIN_SIZE: the frozen minimum a caller must supply. Growing a
 *   struct must NOT move its MIN_SIZE (that would be an incompatible change). */
#define GPTPS_STRUCT_HAS(type, ptr, field) \
    ((ptr)->struct_size >= offsetof(type, field) + sizeof((ptr)->field))
/* gptps_task_def froze at the v1.11 prefix: everything through child_setup is
 * required; `flags` (v1.11) and anything appended later is optional. `flags` is a
 * uint64_t and the struct is 8-aligned, so offsetof(flags) == the pre-v1.11 padded
 * sizeof on every ABI (the field sits exactly at the old size boundary, never inside
 * old trailing padding) - so this equals the pre-v1.11 sizeof(gptps_task_def) and
 * every existing caller still validates, while GPTPS_STRUCT_HAS(flags) stays a true
 * distinguisher (a pre-v1.11 struct_size is strictly below its +8 threshold). */
#define GPTPS_TASK_DEF_MIN_SIZE (offsetof(gptps_task_def, flags))
/* Compile-time guard against the tail-padding hazard: `flags` must stay the struct's
 * widest (hence most-aligned) member, so it lands exactly at the pre-v1.11 padded
 * size boundary and never inside old trailing padding (which would let a pre-v1.11
 * struct_size be misread as "has flags"). Enforced portably by keeping it 8 bytes: a
 * uint64_t is the widest scalar in this struct, so on EVERY ABI - i386 (4-aligned
 * uint64) through ARM32/AAPCS (8-aligned) and s390x - it sits at offsetof == the
 * predecessor sizeof. A uint32_t would drop this to 4 and reintroduce the hazard.
 * (Checking `offsetof(flags) % 8` would be wrong: it false-fails on i386, where the
 * struct is only 4-aligned yet perfectly safe.) C99 negative-array-size assertion. */
typedef char gptps__task_def_flags_is_widest[(sizeof(((gptps_task_def *)0)->flags) == 8u) ? 1 : -1];

/* Frozen minimums for the other caller-supplied INPUT structs: the point just past
 * the current LAST field. This does NOT track sizeof (unlike `< sizeof *x`), so
 * appending a field later leaves the minimum where it is and a caller compiled
 * against today's header keeps validating - i.e. it is the append-safe floor. A
 * field appended past here is read via GPTPS_STRUCT_HAS and, to stay unambiguous,
 * must GROW sizeof (not hide in trailing padding - see the task_def note above). */
#define GPTPS_LAST_FIELD_END(type, last) (offsetof(type, last) + sizeof(((type *)0)->last))
#define GPTPS_CONFIG_MIN_SIZE         GPTPS_LAST_FIELD_END(gptps_config, mode)
#define GPTPS_SUBMIT_OPTIONS_MIN_SIZE GPTPS_LAST_FIELD_END(gptps_submit_options, timeout_ms)
#define GPTPS_ALLOCATOR_MIN_SIZE      GPTPS_LAST_FIELD_END(gptps_allocator, user_data)
#define GPTPS_ADDON_MIN_SIZE          GPTPS_LAST_FIELD_END(gptps_addon, teardown)
/* gptps_addon_info is an OUTPUT struct, but the same rule applies for the same
 * reason: validating it with `< sizeof *out` would pin it to today's size, so the
 * day a field is appended every already-compiled caller starts getting E_INVAL. That
 * is exactly the trap this whole section exists to avoid, and it is free to avoid
 * only while the struct is unreleased.
 *
 * gptps_task_info shipped in 1.0.0 with the older `< sizeof` check and was left that
 * way until ABI 2.2 needed to append `flags` to it - at which point the old check WAS
 * the trap, and would have started returning E_INVAL to every caller already compiled.
 * Retrofitting the floor is strictly more permissive (anyone passing sizeof still
 * passes), so it breaks nobody and unblocks the append. Its floor is frozen at the
 * 1.0.0 layout, which ends at `dead`. gptps_setting_info still carries the old check;
 * it can be retrofitted the same way the day something needs to grow it. */
#define GPTPS_ADDON_INFO_MIN_SIZE     GPTPS_LAST_FIELD_END(gptps_addon_info, enabled)
#define GPTPS_TASK_INFO_MIN_SIZE      GPTPS_LAST_FIELD_END(gptps_task_info, dead)
/* Same tail-padding hazard as gptps_task_def above, same remedy: `flags` must stay the
 * widest member so offsetof(flags) lands exactly on the pre-2.2 padded sizeof and can
 * never hide inside old trailing padding - otherwise a 1.0.0 struct_size could be
 * misread as "has flags". C99 negative-array-size assertion. */
typedef char gptps__task_info_flags_is_widest[(sizeof(((gptps_task_info *)0)->flags) == 8u) ? 1 : -1];

/* --- core allocator seam (alloc.c) ---
 * Every CORE allocation goes through these; they default to the C library and
 * are redirected process-wide by the public gptps_set_allocator(). gptps_free
 * tolerates NULL; gptps_calloc guards size overflow. (The HAL uses libc directly
 * - it is replaced wholesale on exotic targets.) */
void *gptps_malloc(size_t size);
void *gptps_calloc(size_t n, size_t size);
void *gptps_realloc(void *ptr, size_t size);
void  gptps_free(void *ptr);

/* config model + auto-tune (T6).
 * Resolves a caller's limits against detected hardware:
 *   - max_concurrent_tasks == 0  => detected CPU count
 *   - max_memory_bytes     == 0  => 0.75 * detected RAM (floor if RAM unknown)
 * Any explicit non-zero value is passed through unchanged (explicit wins).
 * `in` may be NULL (treated as "all auto"). */
gptps_status gptps_config_resolve(const gptps_limits *in, gptps_limits *out);

/* Build a ctx, run the task in THIS process, and hand out a BORROWED pointer to its
 * result bytes (NULL/0 if none). The caller must NOT free them: this is called only
 * in the forked OOP child, which must not touch the allocator (a host allocator's
 * lock may have been held by a thread that did not survive fork()) and which _exit()s
 * immediately after writing the bytes to its pipe. */
gptps_status gptps_run_capture(const gptps_task_def *def, const void *payload, size_t plen,
                               void **out_result, size_t *out_len);

/* Out-of-process executor (POSIX): fork, apply an OS memory cap in the child,
 * run the task there, and stream the result back. The parent hard-kills the
 * child on timeout (returns GPTPS_E_TIMEOUT) - real enforcement the in-process
 * path cannot provide. The memory cap is accurate cgroup v2 (memory.max +
 * swap.max=0, exceeding it => GPTPS_E_NOMEM) when GPTPS_CGROUP_PARENT names a
 * memory-delegated cgroup; otherwise a coarse RLIMIT_AS fallback. mem_cap==0 or
 * below a floor => no cap. `cancel` (may be NULL) is the running item's cooperative
 * cancel flag: the parent waits in bounded slices and hard-kills the child when it is
 * raised, so a cancel / shutdown / task-removal stops even a no-timeout (timeout_s==0)
 * child instead of blocking its worker forever.
 * The kill REASON is reported distinctly: GPTPS_E_CANCELLED for the flag,
 * GPTPS_E_TIMEOUT for the deadline, GPTPS_E_IO for a pump failure - so an operator's
 * cancel is never mistaken for a deadline breach. A child that stops talking without
 * exiting is reaped with a bounded grace period, never an unbounded waitpid(). */
gptps_status gptps_oop_execute(const gptps_task_def *def, const void *payload, size_t plen,
                               uint64_t mem_cap, uint32_t timeout_s, const uint32_t *cancel,
                               void **out_result, size_t *out_len);

/* --- minimal TOML-subset config parser (config_toml.c) --- */
typedef struct gptps_toml gptps_toml;
gptps_toml *gptps_toml_parse_file(const char *path, char *errbuf, size_t errlen); /* NULL on error */
/* Why parse_file_ex or read_file returned NULL, through their `why` (may be NULL; left
 * alone on success): the caller words its own message, and memory running out is not
 * a mistake in the file, which may be fine. */
#define GPTPS_TOML_BAD    1     /* the text: a line that does not parse, a NUL byte */
#define GPTPS_TOML_UNREAD 2     /* the file cannot be opened, sized or read */
#define GPTPS_TOML_NOMEM  3     /* memory ran out */
gptps_toml *gptps_toml_parse_file_ex(const char *path, char *errbuf, size_t errlen, int *why);
/* The same parse of text already in memory; `path` names it in messages. When it fails
 * because memory ran out, *oom (may be NULL) is set to 1; it is left alone otherwise. */
gptps_toml *gptps_toml_parse_text(const char *path, const char *text, char *errbuf, size_t errlen,
                                  int *oom);
/* The file, read with the checks parse_file makes, NUL-terminated (gptps_free it);
 * NULL with errbuf filled, and *why set, when it cannot be. */
char       *gptps_toml_read_file(const char *path, char *errbuf, size_t errlen, int *why);
void        gptps_toml_free(gptps_toml *t);
int         gptps_toml_int(const gptps_toml *t, const char *section, const char *key, long long *out);
int         gptps_toml_double(const gptps_toml *t, const char *section, const char *key, double *out);
int         gptps_toml_bool(const gptps_toml *t, const char *section, const char *key, int *out);
const char *gptps_toml_str(const gptps_toml *t, const char *section, const char *key);
int         gptps_toml_str_array(const gptps_toml *t, const char *section, const char *key, const char *const **out);
/* Entry-wise access for the loader (docs/CONFIG.md): every entry keeps its line,
 * its value as the text the settings registry validates (NULL for an array), and
 * whether a consumer has claimed it. A dotted key is section + "." + key wherever
 * the dots fall, so [a.b] c = 1 and [a] "b.c" = 1 are both a.b.c. */
const char *gptps_toml_path(const gptps_toml *t);
size_t      gptps_toml_count(const gptps_toml *t);
const char *gptps_toml_section_at(const gptps_toml *t, size_t i);
const char *gptps_toml_key_at(const gptps_toml *t, size_t i);
const char *gptps_toml_text_at(const gptps_toml *t, size_t i);
int         gptps_toml_line_at(const gptps_toml *t, size_t i);
int         gptps_toml_claimed_at(const gptps_toml *t, size_t i);
void        gptps_toml_claim_at(gptps_toml *t, size_t i);
/* Whether the engine refused the entry's value - at the reload that read the file, or
 * when the setting it names was defined since. A save that copies the file writes the
 * engine's own value there instead (gptps_settings_save_to). */
int         gptps_toml_refused_at(const gptps_toml *t, size_t i);
void        gptps_toml_refuse_at(gptps_toml *t, size_t i);
void        gptps_toml_dotted_at(const gptps_toml *t, size_t i, char *buf, size_t cap);
/* section + "." + key (just key when section is ""), for a message: whole if it fits
 * `cap`, else its start and its end with "..." between, so what follows the key in
 * the message is not cut off. GPTPS_TOML_SHOWN is the size the messages use. */
#define GPTPS_TOML_SHOWN 160
void        gptps_toml_key_shown(const char *section, const char *key, char *buf, size_t cap);
long        gptps_toml_find_dotted(const gptps_toml *t, const char *dotted);   /* -1 if none */
/* What a value is, as the file wrote it: the loader holds it to its setting's type. */
typedef enum { GPTPS_TOML_INT, GPTPS_TOML_FLOAT, GPTPS_TOML_BOOL, GPTPS_TOML_STRING, GPTPS_TOML_ARRAY } gptps_toml_kind;
gptps_toml_kind gptps_toml_kind_at(const gptps_toml *t, size_t i);
/* Every [table] line, in file order: its line and its name, quoted parts unquoted. */
size_t      gptps_toml_table_count(const gptps_toml *t);
int         gptps_toml_table_line_at(const gptps_toml *t, size_t i);
const char *gptps_toml_table_at(const gptps_toml *t, size_t i);

/* --- settings registry (settings.c) --- */
typedef struct gptps_settings gptps_settings;
gptps_settings *gptps_settings_create(void);
void            gptps_settings_destroy(gptps_settings *r);
gptps_status    gptps_settings_add(gptps_settings *r, const gptps_setting_def *def);
/* As gptps_settings_add, recording `owner` (a task type's reg) for
 * gptps_settings_remove_task. */
gptps_status    gptps_settings_add_owned(gptps_settings *r, const gptps_setting_def *def,
                                         const void *owner);
size_t          gptps_settings_size(gptps_settings *r);
gptps_status    gptps_settings_get_by(gptps_settings *r, const char *key, char *buf, size_t cap);
gptps_status    gptps_settings_set_by(gptps_settings *r, const char *key, const char *value);
gptps_status    gptps_settings_info_at(gptps_settings *r, size_t index, gptps_setting_info *out);
/* Save (gptps_settings_save): a file that exists is updated in place with the
 * values set live. A new one starts as a copy of `base` - the config file the
 * engine loaded, or NULL - updated the same way; without a usable `base`, it gets
 * the live values and the ones the loaded file set. Only values the engine took go
 * into a new file from the loaded one:
 *   - `in_file` says what the loaded file gives a setting: 1 with its value (as text,
 *     unquoted) in `val`; 2 when it gives one the engine refused, so the setting's own
 *     value is written instead; or 0;
 *   - `refused` is asked of each key a copy of `base` holds: 1 when the engine refused
 *     the value `text` (NULL: a list) the loaded file gives `key`. The copy then
 *     writes the setting's own value, or leaves the key out if no setting has it.
 * Either may be NULL. Both are called with the settings lock held. */
typedef int (*gptps_settings_in_file_fn)(const char *key, void *ud, char *val, size_t cap);
typedef int (*gptps_settings_refused_fn)(const char *key, const char *text, void *ud);
gptps_status    gptps_settings_save_to(gptps_settings *r, const char *path, const char *base,
                                       gptps_settings_in_file_fn in_file,
                                       gptps_settings_refused_fn refused, void *ud);
/* A number as a config file writes it (sign, digits; for !whole a fraction and an
 * exponent): what every numeric setting takes, so a saved value reads back. */
int             gptps_settings_plain_number(const char *v, int whole);
/* strtod and %.17g as the C locale has them, whatever LC_NUMERIC the host set. */
double          gptps_strtod_c(const char *s, char **end);
void            gptps_fmt_double_c(char *buf, size_t cap, double v);
/* set_by and set_live are live sets; set_text is a file's. The last two say why on
 * GPTPS_E_CONFIG. */
gptps_status    gptps_settings_set_live(gptps_settings *r, const char *key, const char *value,
                                        char *why, size_t whylen);
gptps_status    gptps_settings_set_text(gptps_settings *r, const char *key, const char *value,
                                        char *why, size_t whylen);
void            gptps_settings_nosave(gptps_settings *r, const char *key);  /* a statistic: never saved */
int             gptps_settings_has(gptps_settings *r, const char *key);
int             gptps_settings_type_of(gptps_settings *r, const char *key, gptps_setting_type *out);
int             gptps_settings_closest(gptps_settings *r, const char *key, char *buf, size_t cap);
size_t          gptps_edit_distance(const char *a, const char *b, size_t cap);
void            gptps_settings_explain(gptps_setting_type type, int has_range, double min, double max,
                                       const char *const *choices, const char *v, char *why, size_t cap);
/* A watcher hears the live sets; with `files`, a config file's values too (an
 * add-on's watcher, registered through its host table). `tag` names the add-on
 * load that registers a watcher or a setting while its setup runs (else NULL);
 * forget_tag switches off that load's watchers and removes its settings, when its
 * setup fails. */
gptps_status    gptps_settings_watch_add(gptps_settings *r, gptps_settings_cb cb, void *ud, int files,
                                         const void *tag);
void            gptps_settings_set_tag(gptps_settings *r, const char *key, const void *tag);
void            gptps_settings_forget_tag(gptps_settings *r, const void *tag);
/* Tear down an unregistered task type's settings: every entry added with this
 * `owner` (the engine's per-task knobs and defined leaves), and - when `prefix` is
 * given - every UNOWNED entry under it (a host's gptps_register_setting key under
 * "tasks.<name>.", removed at unregister as it always was) unless it also lies
 * under one of the `keep` prefixes. By owner, not by prefix alone: a prefix removal
 * of "tasks.resize." also took the keys of a live type named "resize.big", and a
 * namespaced leaf such as "gpuq.units" means a key's shape cannot tell them apart;
 * `keep` holds the live siblings' "tasks.<sibling>." prefixes for the host keys.
 * Returns the count removed. Takes only the settings lock (never the engine lock),
 * so callers must NOT hold the engine lock (preserve settings->m -> engine->m). */
size_t          gptps_settings_remove_task(gptps_settings *r, const void *owner, const char *prefix,
                                           const char *const *keep, size_t nkeep);
/* Settings made first and published at once, for a call that makes many and must make
 * all of them or none: a task's registration, gptps_define_resource,
 * gptps_define_task_setting. prepare() allocates everything one setting needs - its
 * entry, key, description and default - and nothing can see it yet. It renders the
 * default with def->read unless `defval` gives it, so the caller holds no lock (the
 * read takes the target's own). publish_locked() links it into the registry and
 * allocates nothing, so a batch of them cannot fail part-way: it returns 1, or 0 when
 * the key is someone else's already, which stays theirs, and `p` is then still the
 * caller's to free. The caller holds the registry's lock (gptps_settings_lock), and
 * may take the engine's under it - settings->m comes first - to publish its own state
 * in the same moment. A prepared setting never published goes with prep_free. */
typedef struct gptps_setting_entry gptps_setting_prep;
gptps_status    gptps_settings_prepare(const gptps_setting_def *def, const void *owner,
                                       const char *defval, gptps_setting_prep **out);
void            gptps_settings_prep_free(gptps_setting_prep *p);
void            gptps_settings_lock(gptps_settings *r);
void            gptps_settings_unlock(gptps_settings *r);
int             gptps_settings_publish_locked(gptps_settings *r, gptps_setting_prep *p);

/* Out-of-process EXTERNAL PROGRAM executor (POSIX): fork + exec argv[0] under an
 * OS memory cap, feed `payload` on the child's stdin, read its stdout as the
 * result, hard-kill on the deadline. Exit 0 => OK, non-zero => GPTPS_E_TASK. The
 * parent pumps stdin and stdout CONCURRENTLY (a single poll loop), so a large
 * payload through a streaming child does not deadlock. `cancel` (may be NULL) is
 * the item's cooperative cancel flag - the pump kills the child when it is raised,
 * so a no-timeout program can still be cancelled / shut down (reported as
 * GPTPS_E_CANCELLED, distinct from a deadline's GPTPS_E_TIMEOUT). Stdout EOF means the
 * child closed its output, NOT that it exited, so the reap is bounded: a program that
 * goes silent and keeps running is SIGKILLed after a grace period. */
gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s, const uint32_t *cancel,
                                   void **out_result, size_t *out_len);

#endif /* GPTPS_INTERNAL_H */
