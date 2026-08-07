/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_addon_ns.c - add-on IDENTITY (ABI 2.1): namespace claim + enforcement,
 * introspection, disable, and scheduler-seam ownership.
 *
 * These are the properties an ECOSYSTEM needs and a single-host setup never
 * exercises: two unrelated plug-ins must not be able to collide silently, and an
 * operator must be able to see what is loaded and turn one off.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#ifndef ADDON_NS_PATH
#define ADDON_NS_PATH     "./addon_ns.so"
#endif
#ifndef ADDON_NS_BAD_PATH
#define ADDON_NS_BAD_PATH "./addon_ns_bad.so"
#endif
#ifndef ADDON_DEMO_PATH
#define ADDON_DEMO_PATH   "./addon_demo.so"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int64_t host_score(const gptps_sched_input *in, void *ud)
{ (void)ud; return (int64_t)in->priority; }

int main(void)
{
    gptps *e = NULL;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) { printf("open failed\n"); return 1; }

    /* ===== 1) a namespaced add-on loads and registers under its prefix ===== */
    CHECK(gptps_load_addon(e, ADDON_NS_PATH) == GPTPS_OK);
    CHECK(gptps_task_exists(e, "nstest.work") == 1);
    {
        char b[GPTPS_SETTINGS_VALUE_MAX];
        CHECK(gptps_settings_get(e, "nstest.level", b, sizeof b) == GPTPS_OK);
        CHECK(strcmp(b, "3") == 0);
    }
    {   /* the prefixed per-task leaf materialized on the add-on's own task */
        char b[GPTPS_SETTINGS_VALUE_MAX];
        CHECK(gptps_settings_get(e, "tasks.nstest.work.nstest.units", b, sizeof b) == GPTPS_OK);
    }
    {   /* the namespaced resource exists with the declared budget */
        uint64_t reserved = 0, budget = 0;
        CHECK(gptps_resource_usage(e, "nstest.slots", &reserved, &budget) == GPTPS_OK);
        CHECK(budget == 8);
    }

    /* ===== 2) it took the scheduler seam, and says who it is ===== */
    CHECK(gptps_scheduler_owner(e) != NULL);
    CHECK(gptps_scheduler_owner(e) && strcmp(gptps_scheduler_owner(e), "nstest") == 0);

    /* A polite second installer is REFUSED and the incumbent keeps ordering. Before
     * 2.1 this silently replaced, and both parties believed they held the seam. */
    CHECK(gptps_set_scheduler_ex(e, host_score, NULL, "someone_else", 0) == GPTPS_E_BUSY);
    CHECK(gptps_scheduler_owner(e) && strcmp(gptps_scheduler_owner(e), "nstest") == 0);

    /* ===== 3) the namespace token is CLAIMED: nobody else may have it =====
     * Loading the same library again is the cleanest way to ask for a token that is
     * already taken. */
    CHECK(gptps_load_addon(e, ADDON_NS_PATH) == GPTPS_E_DUP);
    CHECK(gptps_addon_count(e) == 1);            /* the refused load left nothing */

    /* ===== 4) introspection reports what is loaded ===== */
    {
        gptps_addon_info info;
        memset(&info, 0, sizeof info);
        info.struct_size = sizeof info;
        CHECK(gptps_addon_get_info(e, 0, &info) == GPTPS_OK);
        CHECK(info.ns && strcmp(info.ns, "nstest") == 0);
        CHECK(info.name && strcmp(info.name, "ns test") == 0);
        CHECK(info.path && strstr(info.path, "addon_ns") != NULL);
        CHECK(info.abi_version_major == GPTPS_ABI_VERSION_MAJOR);
        CHECK(info.enabled == 1);
        CHECK(gptps_addon_get_info(e, 99, &info) == GPTPS_E_NOTFOUND);
        info.struct_size = 1;                     /* below the floor */
        CHECK(gptps_addon_get_info(e, 0, &info) == GPTPS_E_INVAL);
    }

    /* ===== 5) disable: stops participating, stays mapped ===== */
    CHECK(gptps_addon_disable(e, "nstest") == GPTPS_OK);
    CHECK(gptps_task_exists(e, "nstest.work") == 0);   /* it unregistered its task */
    CHECK(gptps_scheduler_owner(e) == NULL);           /* and released the seam */
    CHECK(gptps_addon_disable(e, "nstest") == GPTPS_OK);       /* idempotent */
    CHECK(gptps_addon_disable(e, "no_such_addon") == GPTPS_E_NOTFOUND);
    {
        gptps_addon_info info;
        memset(&info, 0, sizeof info);
        info.struct_size = sizeof info;
        CHECK(gptps_addon_get_info(e, 0, &info) == GPTPS_OK);
        CHECK(info.enabled == 0);
    }
    /* With the seam released, the host can install its own. */
    CHECK(gptps_set_scheduler_ex(e, host_score, NULL, "host", 0) == GPTPS_OK);
    CHECK(gptps_scheduler_owner(e) && strcmp(gptps_scheduler_owner(e), "host") == 0);

    /* An add-on with no disable hook is refused, and stays enabled. */
    CHECK(gptps_load_addon(e, ADDON_DEMO_PATH) == GPTPS_OK);
    CHECK(gptps_addon_disable(e, "demo") == GPTPS_E_INVAL);

    /* ===== 5b) a FAILED load must not steal the seam's owner label =====
     * addon_unwind restores sched_fn, and used to leave sched_owner pointing at the
     * add-on that just failed. The incumbent's function then ran under a stranger's
     * name: gptps_scheduler_owner reported an add-on that is not installed, and the
     * real owner could no longer release its own seam, because set_scheduler_ex's
     * "am I the owner?" test compares owner strings and saw someone else. Exactly the
     * "two parties both believe they hold it" failure the ownership rule exists to
     * prevent, reintroduced through the failure path. */
    {
        /* host owns the seam... */
        CHECK(gptps_set_scheduler_ex(e, host_score, NULL, "host", GPTPS_SCHED_REPLACE) == GPTPS_OK);
        CHECK(gptps_scheduler_owner(e) && strcmp(gptps_scheduler_owner(e), "host") == 0);
        /* ...a load fails after touching things (the ns-violating add-on) ... */
        CHECK(gptps_load_addon(e, ADDON_NS_BAD_PATH) == GPTPS_E_INVAL);
        /* ...and the host is still the owner, and can still release. */
        CHECK(gptps_scheduler_owner(e) && strcmp(gptps_scheduler_owner(e), "host") == 0);
        CHECK(gptps_set_scheduler_ex(e, NULL, NULL, "host", 0) == GPTPS_OK);
        CHECK(gptps_scheduler_owner(e) == NULL);
    }

    /* ===== 5c) gptps_addon_info validates against a FROZEN FLOOR, not sizeof =====
     * The distinction is the whole append-safe convention: at the floor a caller is
     * served even though its struct is smaller than today's sizeof, which is what
     * lets the struct grow later without invalidating every already-compiled caller.
     * `< sizeof *out` would pin it to today's size forever. */
    {
        gptps_addon_info info;
        const size_t floor = offsetof(gptps_addon_info, enabled)
                           + sizeof(((gptps_addon_info *)0)->enabled);
        memset(&info, 0, sizeof info);
        info.struct_size = floor;
        CHECK(gptps_addon_get_info(e, 0, &info) == GPTPS_OK);        /* at the floor: served */
        info.struct_size = floor - 1;
        CHECK(gptps_addon_get_info(e, 0, &info) == GPTPS_E_INVAL);   /* below it: refused */
    }

    gptps_shutdown(e);

    /* ===== 6) a namespace violation fails the load AND unwinds cleanly ===== */
    e = NULL;
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) { printf("open failed\n"); return 1; }
    CHECK(gptps_load_addon(e, ADDON_NS_BAD_PATH) == GPTPS_E_INVAL);
    /* The unprefixed task was refused... */
    CHECK(gptps_task_exists(e, "unprefixed") == 0);
    /* ...and so was the correctly-prefixed one it managed to register FIRST. A
     * half-installed add-on is worse than either outcome, so the unwind takes both. */
    CHECK(gptps_task_exists(e, "nsbad.fine") == 0);
    CHECK(gptps_addon_count(e) == 0);
    gptps_shutdown(e);

    if (fails) { printf("%d addon-ns check(s) FAILED\n", fails); return 1; }
    printf("all addon namespace/identity checks passed\n");
    return 0;
}
