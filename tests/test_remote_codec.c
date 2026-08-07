/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_remote_codec.c - the remote wire protocol, tested before any socket exists.
 *
 * A wire format is a forever-contract with no compiler to enforce it and no way to
 * recall a deployed peer, so the bytes get tested on their own: exact layout, byte
 * order, every rejection path, and the arithmetic an attacker would aim at.
 *
 * The byte-order checks assert the ACTUAL BYTES, not a round trip. A round trip
 * passes just as happily on a native-endian codec - which is precisely the bug this
 * protocol exists to avoid, and which CI's big-endian s390x leg would otherwise be
 * the only thing standing between us and.
 */
#include "gptps_remote.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main(void)
{
    unsigned char hdr[GPTPS_REMOTE_HDR_SIZE];
    gptps_remote_hdr h, d;

    /* ===== 1) the header is EXACTLY the bytes we promised ===== */
    h.magic = GPTPS_REMOTE_MAGIC; h.version = GPTPS_REMOTE_VERSION;
    h.kind = GPTPS_REMOTE_REQUEST; h.req_id = 0x0102030405060708ull;
    h.status = 0x11223344u; h.payload_len = 0xAABBCCDDu;
    gptps_remote_encode_hdr(&h, hdr);

    /* magic "GPTR", big-endian */
    CHECK(hdr[0] == 0x47 && hdr[1] == 0x50 && hdr[2] == 0x54 && hdr[3] == 0x52);
    CHECK(hdr[4] == 0x00 && hdr[5] == 0x01);                       /* version */
    CHECK(hdr[6] == 0x00 && hdr[7] == 0x01);                       /* kind = REQUEST */
    /* req_id: most-significant byte FIRST. On a little-endian host a memcpy would
     * have put 0x08 here, which is the whole point of checking bytes. */
    CHECK(hdr[8] == 0x01 && hdr[15] == 0x08);
    CHECK(hdr[16] == 0x11 && hdr[19] == 0x44);                     /* status */
    CHECK(hdr[20] == 0xAA && hdr[23] == 0xDD);                     /* payload_len */

    /* ...and it round-trips */
    CHECK(gptps_remote_decode_hdr(hdr, sizeof hdr, 0xFFFFFFFFu, &d) == GPTPS_OK);
    CHECK(d.magic == h.magic && d.version == h.version && d.kind == h.kind);
    CHECK(d.req_id == h.req_id && d.status == h.status && d.payload_len == h.payload_len);

    /* ===== 2) every rejection path ===== */
    {
        unsigned char b[GPTPS_REMOTE_HDR_SIZE];

        CHECK(gptps_remote_decode_hdr(hdr, GPTPS_REMOTE_HDR_SIZE - 1, 0, &d) == GPTPS_E_INVAL);
        CHECK(gptps_remote_decode_hdr(NULL, sizeof hdr, 0, &d) == GPTPS_E_INVAL);
        CHECK(gptps_remote_decode_hdr(hdr, sizeof hdr, 0, NULL) == GPTPS_E_INVAL);

        memcpy(b, hdr, sizeof b); b[0] = 0xFF;                      /* wrong magic */
        CHECK(gptps_remote_decode_hdr(b, sizeof b, 0xFFFFFFFFu, &d) == GPTPS_E_INVAL);

        memcpy(b, hdr, sizeof b); b[5] = 99;                        /* unknown version */
        CHECK(gptps_remote_decode_hdr(b, sizeof b, 0xFFFFFFFFu, &d) == GPTPS_E_INVAL);

        memcpy(b, hdr, sizeof b); b[7] = 77;                        /* unknown kind */
        CHECK(gptps_remote_decode_hdr(b, sizeof b, 0xFFFFFFFFu, &d) == GPTPS_E_INVAL);

        /* THE cap. A length is the cheapest thing an unauthenticated peer can lie
         * about, so it must be refused BEFORE anyone allocates or reads. */
        gptps_remote_make_request(&h, 1, GPTPS_REMOTE_MAX_PAYLOAD + 1);
        gptps_remote_encode_hdr(&h, b);
        CHECK(gptps_remote_decode_hdr(b, sizeof b, 0, &d) == GPTPS_E_INVAL);   /* default cap */
        gptps_remote_make_request(&h, 1, GPTPS_REMOTE_MAX_PAYLOAD);
        gptps_remote_encode_hdr(&h, b);
        CHECK(gptps_remote_decode_hdr(b, sizeof b, 0, &d) == GPTPS_OK);        /* exactly at it */
        /* and a smaller per-link cap is honoured */
        CHECK(gptps_remote_decode_hdr(b, sizeof b, 4096, &d) == GPTPS_E_INVAL);
    }

    /* ===== 3) status mapping is stable and total ===== */
    {
        static const gptps_status all[] = {
            GPTPS_OK, GPTPS_E_INVAL, GPTPS_E_NOTFOUND, GPTPS_E_BUDGET, GPTPS_E_TIMEOUT,
            GPTPS_E_CANCELLED, GPTPS_E_IO, GPTPS_E_NOMEM, GPTPS_E_DENIED,
            GPTPS_E_FULL, GPTPS_E_SHUTDOWN
        };
        size_t i;
        for (i = 0; i < sizeof all / sizeof all[0]; ++i)
            CHECK(gptps_remote_status_from_wire(gptps_remote_status_to_wire(all[i])) == all[i]);

        /* The wire codes are FIXED. If someone renumbers the enum, these fail - which
         * is the point: a deployed peer cannot be recalled. */
        CHECK(gptps_remote_status_to_wire(GPTPS_OK) == 0);
        CHECK(gptps_remote_status_to_wire(GPTPS_E_CANCELLED) == 5);
        CHECK(gptps_remote_status_to_wire(GPTPS_E_SHUTDOWN) == 10);

        /* A code from a newer peer degrades to a plain failure rather than being
         * reinterpreted as whatever that number means locally. */
        CHECK(gptps_remote_status_from_wire(4242) == GPTPS_E_IO);
        CHECK(gptps_remote_status_from_wire(GPTPS_RW_UNKNOWN) == GPTPS_E_IO);
    }

    /* ===== 4) request payload framing ===== */
    {
        unsigned char buf[256];
        size_t n = sizeof buf;
        const char *task; size_t tlen;
        const void *item; size_t ilen;

        CHECK(gptps_remote_encode_request("resize", "hello", 5, buf, &n) == GPTPS_OK);
        CHECK(n == 4 + 6 + 5);
        CHECK(gptps_remote_decode_request(buf, n, &task, &tlen, &item, &ilen) == GPTPS_OK);
        CHECK(tlen == 6 && memcmp(task, "resize", 6) == 0);
        CHECK(ilen == 5 && item && memcmp(item, "hello", 5) == 0);

        /* an empty item is legal and yields NULL, not a pointer to nothing */
        n = sizeof buf;
        CHECK(gptps_remote_encode_request("t", NULL, 0, buf, &n) == GPTPS_OK);
        CHECK(gptps_remote_decode_request(buf, n, &task, &tlen, &item, &ilen) == GPTPS_OK);
        CHECK(tlen == 1 && ilen == 0 && item == NULL);

        /* too small a buffer reports the size needed rather than truncating */
        n = 3;
        CHECK(gptps_remote_encode_request("resize", "hello", 5, buf, &n) == GPTPS_E_BUDGET);
        CHECK(n == 4 + 6 + 5);

        /* THE attack: a task_len that runs past the frame */
        n = sizeof buf;
        CHECK(gptps_remote_encode_request("resize", "hello", 5, buf, &n) == GPTPS_OK);
        buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0xFF;   /* tlen = 4G */
        CHECK(gptps_remote_decode_request(buf, n, &task, &tlen, &item, &ilen) == GPTPS_E_INVAL);
        /* and the off-by-one either side of the boundary */
        buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = (unsigned char)(n - 4);
        CHECK(gptps_remote_decode_request(buf, n, &task, &tlen, &item, &ilen) == GPTPS_OK);
        buf[3] = (unsigned char)(n - 3);                              /* one past the end */
        CHECK(gptps_remote_decode_request(buf, n, &task, &tlen, &item, &ilen) == GPTPS_E_INVAL);

        /* truncated frames */
        CHECK(gptps_remote_decode_request(buf, 3, &task, &tlen, &item, &ilen) == GPTPS_E_INVAL);
        CHECK(gptps_remote_decode_request(buf, 0, &task, &tlen, &item, &ilen) == GPTPS_E_INVAL);
    }

    if (fails) { printf("%d remote-codec check(s) FAILED\n", fails); return 1; }
    printf("all remote codec checks passed\n");
    return 0;
}
