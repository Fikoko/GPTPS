/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_hal_posix.c - what the bundled POSIX HAL (src/hal_posix.c) does beyond the
 * contract test_hal_conformance holds every HAL to.
 *
 *  1) Linux: auto-tune sizes to the cgroup this process is IN - found through
 *     /proc/self/cgroup and /proc/self/mountinfo - and to every cgroup above it,
 *     cgroup v2 and v1: a systemd service's MemoryMax=, a slice's limit, a Docker
 *     container on a v1 host with or without its own cgroup namespace. Checked on a
 *     tree of this test's own, through gptps__cgroup_clamp_at.
 *  2) The settings save's replace keeps the target's permissions without a window:
 *     a descriptor opened on the temp file while it was written - more open than the
 *     target, under the umask - does not reach the new config.
 *  3) Linux: the replace is durable - the new file and its directory are synced
 *     (fsync is interposed here and counted).
 *  4) The temp file is private from its creation (gptps_hal_open_private), whatever
 *     the umask: nobody else can open it to read the settings or to write into it
 *     before the replace copies it - and gptps_settings_save writes through it.
 *
 * Built only with the bundled HAL (not with -DGPTPS_HAL_SOURCE).
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE                 /* syscall() for the interposed fsync */
#endif
#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_hal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#  include <sys/syscall.h>
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* ---- 3) fsync, counted: this definition wins over libc's for the static library */
#if defined(__linux__) && defined(SYS_fsync)
#  define COUNT_FSYNC 1
static int g_fsync_file, g_fsync_dir;
int fsync(int fd)
{
    struct stat st;
    if (fstat(fd, &st) == 0) { if (S_ISDIR(st.st_mode)) ++g_fsync_dir; else ++g_fsync_file; }
    return (int)syscall(SYS_fsync, fd);
}
#else
#  define COUNT_FSYNC 0
#endif

static int put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(text, f);
    return fclose(f) == 0;
}

static int is(const char *path, const char *text)
{
    char buf[256];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strcmp(buf, text) == 0;
}

/* ---- 1) the cgroup clamp ---------------------------------------------------- */
#if defined(__linux__)
void gptps__cgroup_clamp_at(const char *self_cgroup, const char *mountinfo, const char *prefix,
                            gptps_hwinfo *out);

static char g_root[1024];

static void mkdirs(const char *rel)   /* under g_root, every level */
{
    char path[2048], *p;
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    for (p = path + strlen(g_root) + 1; *p; ++p)
        if (*p == '/') { *p = 0; (void)mkdir(path, 0755); *p = '/'; }
    (void)mkdir(path, 0755);
}

static void putf(const char *rel, const char *text)
{
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    CHECK(put(path, text));
}

static gptps_hwinfo clamp(const char *cgroup, const char *mounts)
{
    gptps_hwinfo hw;
    char cg[2048], mi[2048], prefix[1100];
    memset(&hw, 0, sizeof hw);
    hw.cpu_count = 64;
    hw.ram_bytes = 64ull << 30;
    snprintf(cg, sizeof cg, "%s/self_cgroup", g_root);
    snprintf(mi, sizeof mi, "%s/mountinfo", g_root);
    snprintf(prefix, sizeof prefix, "%s/fs", g_root);
    CHECK(put(cg, cgroup));
    CHECK(put(mi, mounts));
    gptps__cgroup_clamp_at(cg, mi, prefix, &hw);
    return hw;
}

static void rm_tree(void)
{
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    if (system(cmd) != 0) { /* left behind: harmless */ }
}

static void cgroup_clamp(void)
{
    gptps_hwinfo hw;
    if (!getcwd(g_root, sizeof g_root - 64)) { CHECK(0); return; }
    strcat(g_root, "/gptps_hal_posix_tree");
    rm_tree();
    (void)mkdir(g_root, 0755);

    /* a) cgroup v2, no namespace: a systemd service. The root has no limit files;
     * the service's MemoryMax= is on its cgroup, a tighter one on its slice. */
    mkdirs("fs/sys/fs/cgroup/system.slice/erp.service");
    putf("fs/sys/fs/cgroup/system.slice/erp.service/memory.max", "4294967296\n");
    putf("fs/sys/fs/cgroup/system.slice/erp.service/cpu.max", "150000 100000\n");
    putf("fs/sys/fs/cgroup/system.slice/memory.max", "2147483648\n");
    putf("fs/sys/fs/cgroup/system.slice/cpu.max", "max 100000\n");
    hw = clamp("0::/system.slice/erp.service\n",
               "22 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
               "30 22 0:26 / /sys/fs/cgroup rw,nosuid shared:4 - cgroup2 cgroup2 rw,nsdelegate\n");
    CHECK(hw.ram_bytes == 2147483648ull);           /* the slice's: the tightest above */
    CHECK(hw.cpu_count == 2);                       /* 1.5 CPUs, rounded up */

    /* b) a container with its own cgroup namespace: its cgroup is the mount's root */
    putf("fs/sys/fs/cgroup/memory.max", "max\n");
    hw = clamp("0::/\n", "30 22 0:26 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n");
    CHECK(hw.ram_bytes == (64ull << 30) && hw.cpu_count == 64);   /* "max": no limit */
    putf("fs/sys/fs/cgroup/memory.max", "536870912\n");
    hw = clamp("0::/\n", "30 22 0:26 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n");
    CHECK(hw.ram_bytes == 536870912ull);

    /* c) cgroup v1, a Docker container without a namespace of its own: the host's
     * tree, the container's cgroup below it; the root's "no limit" is a huge number */
    mkdirs("fs/sys/fs/cgroup/memory/docker/abc");
    mkdirs("fs/sys/fs/cgroup/cpu,cpuacct/docker/abc");
    putf("fs/sys/fs/cgroup/memory/memory.limit_in_bytes", "9223372036854771712\n");
    putf("fs/sys/fs/cgroup/memory/docker/abc/memory.limit_in_bytes", "1073741824\n");
    putf("fs/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us", "-1\n");
    putf("fs/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us", "100000\n");
    putf("fs/sys/fs/cgroup/cpu,cpuacct/docker/abc/cpu.cfs_quota_us", "50000\n");
    putf("fs/sys/fs/cgroup/cpu,cpuacct/docker/abc/cpu.cfs_period_us", "100000\n");
    hw = clamp("12:pids:/docker/abc\n5:memory:/docker/abc\n3:cpu,cpuacct:/docker/abc\n0::/\n",
               "36 22 0:28 / /sys/fs/cgroup rw - tmpfs tmpfs rw,mode=755\n"
               "37 36 0:29 / /sys/fs/cgroup/cpu,cpuacct rw - cgroup cgroup rw,cpu,cpuacct\n"
               "40 36 0:32 / /sys/fs/cgroup/memory rw - cgroup cgroup rw,memory\n");
    CHECK(hw.ram_bytes == 1073741824ull);
    CHECK(hw.cpu_count == 1);

    /* d) cgroup v1, the container's own cgroup mounted as the hierarchy (its mount
     * root is /docker/abc), at a mount point with a space in it (\040) */
    mkdirs("fs/cg v1/memory");
    putf("fs/cg v1/memory/memory.limit_in_bytes", "805306368\n");
    hw = clamp("5:memory:/docker/abc\n",
               "40 36 0:32 /docker/abc /cg\\040v1/memory rw - cgroup cgroup rw,memory\n");
    CHECK(hw.ram_bytes == 805306368ull);
    CHECK(hw.cpu_count == 64);

    /* e) nothing to go by: figures as they were */
    hw = clamp("", "");
    CHECK(hw.ram_bytes == 536870912ull);           /* the conventional /sys/fs/cgroup, as before */
    rm_tree();
}
#endif

/* ---- 2) and 3) the settings save's replace ---------------------------------- */
static void replace(void)
{
    const char *fin = "gptps_hal_posix.toml", *tmp = "gptps_hal_posix.toml.tmp";
    struct stat sf, st, sh;
    int held;
    mode_t old = umask(022);                        /* this test's own process */
    remove(fin); remove(tmp);

    /* 2) the target is kept to its owner; the save's temp file, made with fopen, is
     * world-readable under umask 022, and someone opened it while it was written */
    CHECK(put(fin, "old"));
    CHECK(chmod(fin, 0600) == 0);
    CHECK(put(tmp, "new"));
    held = open(tmp, O_RDWR);
    CHECK(held >= 0);
#if COUNT_FSYNC
    g_fsync_file = g_fsync_dir = 0;
#endif
    CHECK(gptps_hal_atomic_replace(tmp, fin) == GPTPS_OK);
    CHECK(is(fin, "new"));
    CHECK(stat(fin, &sf) == 0 && (sf.st_mode & 07777) == 0600);
    CHECK(stat(tmp, &st) != 0);                     /* the source is gone */
    if (held >= 0) {
        CHECK(fstat(held, &sh) == 0 && sh.st_ino != sf.st_ino);   /* not the config's inode */
        CHECK(write(held, "EVIL", 4) == 4);
        close(held);
        CHECK(is(fin, "new"));                      /* nothing reached it */
    }
#if COUNT_FSYNC
    CHECK(g_fsync_file >= 1);                       /* 3) the data, before the rename */
    CHECK(g_fsync_dir >= 1);                        /*    and the rename itself */
#endif

    /* the temp file was never more open than the target: it simply becomes it */
    CHECK(put(tmp, "newer"));
    CHECK(chmod(tmp, 0600) == 0);
    CHECK(stat(tmp, &st) == 0);
#if COUNT_FSYNC
    g_fsync_file = g_fsync_dir = 0;
#endif
    CHECK(gptps_hal_atomic_replace(tmp, fin) == GPTPS_OK);
    CHECK(is(fin, "newer"));
    CHECK(stat(fin, &sf) == 0 && sf.st_ino == st.st_ino && (sf.st_mode & 07777) == 0600);
#if COUNT_FSYNC
    CHECK(g_fsync_file >= 1 && g_fsync_dir >= 1);
#endif

    /* a group-readable target keeps exactly that */
    CHECK(chmod(fin, 0640) == 0);
    CHECK(put(tmp, "newest"));
    CHECK(gptps_hal_atomic_replace(tmp, fin) == GPTPS_OK);
    CHECK(is(fin, "newest"));
    CHECK(stat(fin, &sf) == 0 && (sf.st_mode & 07777) == 0640);

    /* no target yet: the temp file as it is */
    remove(fin);
    CHECK(put(tmp, "first"));
    CHECK(gptps_hal_atomic_replace(tmp, fin) == GPTPS_OK);
    CHECK(is(fin, "first") && stat(fin, &sf) == 0 && (sf.st_mode & 0777) == 0644);
    CHECK(gptps_hal_atomic_replace(tmp, fin) == GPTPS_E_IO);   /* no source */
    remove(fin);
    umask(old);
}

/* ---- 4) a private temp file ------------------------------------------------ */
#define PRIV_CFG "gptps_hal_posix_save.toml"
static int g_seen_tmp = -1;               /* the temp file's mode while it is written */
static long g_knob = 7;
static size_t knob_read(void *t, char *buf, size_t cap)
{
    struct stat st;
    if (stat(PRIV_CFG ".tmp", &st) == 0) g_seen_tmp = (int)(st.st_mode & 07777);
    return (size_t)snprintf(buf, cap, "%ld", *(long *)t);
}
static gptps_status knob_write(void *t, const char *v) { *(long *)t = atol(v); return GPTPS_OK; }

static void private_tmp(void)
{
    const char *p = "gptps_hal_posix_priv.tmp", *other = "gptps_hal_posix_other.tmp";
    struct stat st, so;
    FILE *f;
    int held;
    mode_t old = umask(002);              /* a group-writable umask, as on many desktops */
    remove(p); remove(other);

    f = gptps_hal_open_private(p);
    CHECK(f != NULL);
    if (f) { CHECK(fputs("x", f) >= 0); CHECK(fclose(f) == 0); }
    CHECK(stat(p, &st) == 0 && (st.st_mode & 07777) == 0600);

    /* one already there, held open by someone: the new file is a new inode */
    held = open(p, O_RDWR);
    CHECK(held >= 0);
    f = gptps_hal_open_private(p);
    CHECK(f != NULL);
    if (f) { CHECK(fputs("new", f) >= 0); CHECK(fclose(f) == 0); }
    if (held >= 0) {
        CHECK(fstat(held, &so) == 0 && stat(p, &st) == 0 && so.st_ino != st.st_ino);
        CHECK(write(held, "EVIL", 4) == 4);
        close(held);
    }
    CHECK(is(p, "new"));

    /* a symlink put there is removed, never written through */
    remove(p);
    CHECK(put(other, "theirs"));
    CHECK(symlink(other, p) == 0);
    f = gptps_hal_open_private(p);
    CHECK(f != NULL);
    if (f) { CHECK(fputs("ours", f) >= 0); CHECK(fclose(f) == 0); }
    CHECK(is(other, "theirs") && is(p, "ours"));
    CHECK(lstat(p, &st) == 0 && S_ISREG(st.st_mode));
    remove(p); remove(other);

    /* and the settings save writes through it */
    {
        gptps *e = NULL;
        gptps_config cfg;
        gptps_setting_def d;
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
        cfg.mode = GPTPS_RUN_MANUAL;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        if (e) {
            memset(&d, 0, sizeof d);
            d.struct_size = sizeof d; d.key = "host.privknob"; d.type = GPTPS_SETTING_INT;
            d.desc = "test"; d.hot = 1; d.target = &g_knob; d.read = knob_read; d.write = knob_write;
            CHECK(gptps_register_setting(e, &d) == GPTPS_OK);
            CHECK(gptps_settings_set(e, "host.privknob", "9") == GPTPS_OK);
            remove(PRIV_CFG);
            g_seen_tmp = -1;
            CHECK(gptps_settings_save(e, PRIV_CFG) == GPTPS_OK);
            CHECK(g_seen_tmp == 0600);    /* never 0664, not even while it was written */
            CHECK(stat(PRIV_CFG, &st) == 0);
            CHECK(gptps_shutdown(e) == GPTPS_OK);
        }
        remove(PRIV_CFG); remove(PRIV_CFG ".tmp");
    }
    umask(old);
}

int main(void)
{
#if defined(__linux__)
    cgroup_clamp();
#endif
    replace();
    private_tmp();
    if (fails) { printf("%d POSIX HAL check(s) FAILED\n", fails); return 1; }
    printf("POSIX HAL: all checks passed\n");
    return 0;
}
