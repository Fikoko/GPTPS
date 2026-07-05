/*
 * test_xport.c - the worker-process transport (scale-out). Proves work submitted
 * here executes in a SEPARATE process (the result carries the worker's pid, which
 * differs from the parent's) and fans out across all worker processes, and that the
 * handler's status and result round-trip correctly. POSIX only (fork + socketpair).
 */
#include "gptps_xport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* handler run IN THE WORKER PROCESS:
 *   "upper" -> [int32 worker-pid][payload upper-cased]   (proves separate process)
 *   "fail"  -> GPTPS_E_TASK, no result
 *   "empty" -> GPTPS_OK, NULL result */
static gptps_status xrun(const char *task, const void *payload, size_t len,
                         void **out, size_t *outlen, void *ud)
{
    (void)ud;
    if (strcmp(task, "fail") == 0)  return GPTPS_E_TASK;
    if (strcmp(task, "empty") == 0) { *out = NULL; *outlen = 0; return GPTPS_OK; }
    {
        int32_t pid = (int32_t)getpid();
        size_t rlen = 4 + len, i;
        unsigned char *r = (unsigned char *)malloc(rlen ? rlen : 1);
        if (!r) return GPTPS_E_NOMEM;
        memcpy(r, &pid, 4);
        for (i = 0; i < len; ++i) {
            unsigned char c = ((const unsigned char *)payload)[i];
            r[4 + i] = (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : c;
        }
        *out = r; *outlen = rlen;
        return GPTPS_OK;
    }
}

int main(void)
{
    gptps_xport *xp;
    int parent = (int)getpid();
    int seen[3]; int nseen = 0;
    int k;

    xp = gptps_xport_open(3, xrun, NULL);
    CHECK(xp); if (!xp) { printf("open failed\n"); return 1; }
    CHECK(gptps_xport_count(xp) == 3);

    /* six round-robin submits => all 3 workers used, each result from a real
     * separate process (distinct pid, none equal to the parent's) */
    for (k = 0; k < 6; ++k) {
        void *res = NULL; size_t rl = 0; gptps_status ts = GPTPS_E_IO;
        gptps_status st = gptps_xport_submit(xp, "upper", "hello", 5, &res, &rl, &ts);
        CHECK(st == GPTPS_OK);
        CHECK(ts == GPTPS_OK);
        CHECK(rl == 9);
        if (res && rl == 9) {
            int32_t wpid; int j, found = 0;
            memcpy(&wpid, res, 4);
            CHECK((int)wpid != parent);                          /* ran out-of-process */
            CHECK(memcmp((char *)res + 4, "HELLO", 5) == 0);     /* transform round-tripped */
            for (j = 0; j < nseen; ++j) if (seen[j] == (int)wpid) found = 1;
            if (!found && nseen < 3) seen[nseen++] = (int)wpid;
        }
        free(res);
    }
    CHECK(nseen == 3);   /* work fanned out across all three worker processes */

    /* handler error status round-trips (transport OK, task status is the error) */
    {
        void *res = NULL; size_t rl = 0; gptps_status ts = GPTPS_OK;
        CHECK(gptps_xport_submit(xp, "fail", NULL, 0, &res, &rl, &ts) == GPTPS_OK);
        CHECK(ts == GPTPS_E_TASK);
        CHECK(res == NULL);
        free(res);
    }
    /* empty result round-trips */
    {
        void *res = (void *)1; size_t rl = 99; gptps_status ts = GPTPS_E_IO;
        CHECK(gptps_xport_submit(xp, "empty", NULL, 0, &res, &rl, &ts) == GPTPS_OK);
        CHECK(ts == GPTPS_OK);
        CHECK(res == NULL);
        CHECK(rl == 0);
    }
    /* bad args */
    CHECK(gptps_xport_submit(NULL, "x", NULL, 0, NULL, NULL, NULL) == GPTPS_E_INVAL);

    gptps_xport_close(xp);   /* returns => all workers reaped cleanly */

    if (fails) { printf("%d xport check(s) FAILED\n", fails); return 1; }
    printf("all xport (scale-out) checks passed\n");
    return 0;
}
