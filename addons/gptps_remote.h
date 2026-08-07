/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_remote.h - the GPTPS remote WIRE PROTOCOL (codec only, for now).
 *
 * addons/gptps_xport carries work to other PROCESSES over a socketpair, and its
 * header says the local socketpair "is the only thing standing between this and
 * cross-MACHINE execution: swap it for a TCP socket and the same protocol reaches
 * another host." That is true of the shape and false of the details, and this file
 * exists to make the details honest before any socket is opened.
 *
 * WHY THE CODEC SHIPS AND IS REVIEWED BEFORE THE TRANSPORT
 *
 * A wire format is a SECOND FOREVER-CONTRACT, standing beside ABI 2.0. Once one
 * peer somewhere speaks version 1, every future version must interoperate with it -
 * and unlike a C ABI there is no compiler to catch a violation, no struct_size to
 * check, and no way to recall a deployed peer. So the bytes are defined, tested and
 * frozen on their own, with no sockets in sight to distract from them.
 *
 * WHAT A NETWORK COMMITS TO THAT A SOCKETPAIR DOES NOT
 *
 *  1. BYTE ORDER. gptps_xport writes raw native-endian integers. Same-machine that
 *     is free; between an x86 client and a big-endian server it is silent
 *     corruption. Everything here is explicitly big-endian, byte by byte - never a
 *     memcpy of an integer, never a cast through a struct pointer (which would also
 *     be an alignment fault on strict targets).
 *  2. gptps_status VALUES BECOME WIRE-VISIBLE. The enum fixes only GPTPS_OK = 0; the
 *     rest are positional and would silently change meaning if anyone ever inserted
 *     one. So the wire carries its OWN stable codes and this file maps between them.
 *     An unknown code from a newer peer degrades to GPTPS_E_IO rather than being
 *     reinterpreted as whatever that number means locally.
 *  3. A REQUEST ID. gptps_xport holds a lock across the whole round trip, so it needs
 *     no correlation. That caps a link at one in-flight request - fine at socketpair
 *     latency, and a hard ceiling of a few thousand/sec over a real network no matter
 *     how fast the far side is. req_id is here so a transport CAN multiplex later.
 *  4. A LENGTH CAP THAT IS A DEFENCE, NOT A SANITY CHECK. gptps_xport allows 256 MiB,
 *     reasonable against your own forked child. From an unauthenticated peer it is a
 *     one-packet memory exhaustion, so the default here is 1 MiB and it is per-link
 *     configurable.
 *
 * SECURITY, STATED PLAINLY: the `task` field of a request is a dispatch key chosen by
 * the peer. On a listening socket that is REMOTE CODE SELECTION by whoever can
 * connect. This protocol has no authentication and no encryption, by design - see
 * docs/SECURITY.md. Run it inside a trusted boundary (loopback, WireGuard, a TLS
 * terminator, an SSH tunnel), never on an open port.
 */
#ifndef GPTPS_REMOTE_H
#define GPTPS_REMOTE_H

#include "gptps.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "GPTR". Present so a peer that is not speaking this protocol at all is rejected
 * on its first frame instead of being interpreted as a length. */
#define GPTPS_REMOTE_MAGIC    0x47505452u

/* Bump ONLY for a change no version-1 peer could parse.
 *
 * BE HONEST ABOUT WHAT v1 CAN AND CANNOT ABSORB. A request payload is
 * [u32 task_len][task][item], where `item` is "everything remaining" - so nothing can
 * ever follow it, and a reply payload has no internal framing at all. That means v1
 * has NO extension slot: a deadline, a tenant id or a trace id cannot be added without
 * a version bump, and this constant is the only lever.
 *
 * That is a deliberate v1 shape (simple, and impossible to get subtly wrong), not an
 * oversight - but it must not be described as though additive fields were possible,
 * because a wire format is a permanent contract and the one thing worse than a narrow
 * format is a narrow format documented as extensible.
 *
 * A v2 that wanted headroom would frame the payload as
 * [u32 n][n x (u16 tag, u32 len, bytes)] and put `task` and `item` in it as tags 1
 * and 2. Do that when something actually needs it - not before. */
#define GPTPS_REMOTE_VERSION  1u

/* Fixed on-wire header size, in bytes. Deliberately a constant rather than a
 * sizeof(): there is no struct, precisely so no compiler's padding or alignment
 * choices can ever reach the wire. */
#define GPTPS_REMOTE_HDR_SIZE 24u

/* Default maximum payload accepted from a peer (1 MiB). See the header note. */
#define GPTPS_REMOTE_MAX_PAYLOAD (1u * 1024u * 1024u)

typedef enum {
    GPTPS_REMOTE_REQUEST  = 1,   /* task + payload  -> run this */
    GPTPS_REMOTE_REPLY    = 2,   /* status + result <- here it is */
    GPTPS_REMOTE_CANCEL   = 3,   /* cancel req_id; no payload */
    GPTPS_REMOTE_PING     = 4    /* liveness; a network gives no EOF when a peer dies */
} gptps_remote_msg_kind;

/* A decoded frame header. Not the wire layout - the wire is bytes, defined by
 * gptps_remote_encode_hdr, and this struct never touches it. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;        /* gptps_remote_msg_kind */
    uint64_t req_id;      /* correlates a REPLY to its REQUEST; 0 = unsolicited */
    uint32_t status;      /* wire status code (REPLY only); see below */
    uint32_t payload_len;
} gptps_remote_hdr;

/* --- status mapping -------------------------------------------------------
 * The wire codes are STABLE and independent of gptps_status's positional values.
 * A code this build does not know maps to GPTPS_E_IO: an unknown failure is still a
 * failure, and guessing would be worse. */
enum {
    GPTPS_RW_OK          = 0,
    GPTPS_RW_INVAL       = 1,
    GPTPS_RW_NOTFOUND    = 2,
    GPTPS_RW_BUDGET      = 3,
    GPTPS_RW_TIMEOUT     = 4,
    GPTPS_RW_CANCELLED   = 5,
    GPTPS_RW_IO          = 6,
    GPTPS_RW_NOMEM       = 7,
    GPTPS_RW_DENIED      = 8,
    GPTPS_RW_FULL        = 9,
    GPTPS_RW_SHUTDOWN    = 10,
    GPTPS_RW_UNKNOWN     = 65535
};
uint32_t     gptps_remote_status_to_wire(gptps_status s);
gptps_status gptps_remote_status_from_wire(uint32_t w);

/* --- header codec ---------------------------------------------------------
 * encode: writes exactly GPTPS_REMOTE_HDR_SIZE bytes into `out`.
 * decode: reads exactly GPTPS_REMOTE_HDR_SIZE bytes from `in`.
 *
 * decode returns:
 *   GPTPS_OK        the header is well-formed and within max_payload
 *   GPTPS_E_INVAL   bad magic, unknown version, unknown kind, or a length above
 *                   max_payload (pass 0 for the default)
 * It does NOT read the payload; the caller does that only after a GPTPS_OK here,
 * which is what makes the length cap a real defence rather than advice. */
void         gptps_remote_encode_hdr(const gptps_remote_hdr *h, unsigned char *out);
gptps_status gptps_remote_decode_hdr(const unsigned char *in, size_t len,
                                     uint32_t max_payload, gptps_remote_hdr *out);

/* Convenience: fill a header for a REQUEST or a REPLY. */
void gptps_remote_make_request(gptps_remote_hdr *h, uint64_t req_id, uint32_t payload_len);
void gptps_remote_make_reply(gptps_remote_hdr *h, uint64_t req_id, gptps_status st,
                             uint32_t payload_len);

/* A REQUEST payload is  [u32 task_len][task bytes][item bytes], so a task name is
 * length-delimited rather than NUL-terminated - a peer must not be able to choose
 * where our string ends.
 *
 * encode_request: needs *len set to the buffer size; sets it to the bytes written.
 *                 GPTPS_E_BUDGET if the buffer is too small.
 * decode_request: returns BORROWED pointers into `buf`, valid as long as it is.
 *                 GPTPS_E_INVAL if the framing does not add up - which includes a
 *                 task_len that overruns the payload, the obvious attack. */
gptps_status gptps_remote_encode_request(const char *task,
                                         const void *item, size_t item_len,
                                         unsigned char *buf, size_t *len);
gptps_status gptps_remote_decode_request(const unsigned char *buf, size_t len,
                                         const char **out_task, size_t *out_task_len,
                                         const void **out_item, size_t *out_item_len);

#ifdef __cplusplus
}
#endif

#endif /* GPTPS_REMOTE_H */
