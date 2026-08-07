/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_remote.c - the GPTPS remote wire codec (see gptps_remote.h).
 *
 * Bytes only. No sockets, no threads, no allocation: every function here is a pure
 * transformation between a caller's buffer and a decoded struct, which is what lets
 * the whole protocol be tested exhaustively before a transport exists to hide bugs
 * behind.
 *
 * Every integer is written and read BYTE BY BYTE in big-endian order. Never a memcpy
 * of an integer, never a cast of the buffer to a struct pointer. Two reasons, and the
 * second is the one people forget:
 *   - byte order: a native-endian wire is silent corruption between a little-endian
 *     client and a big-endian server, which is exactly the axis CI's s390x leg exists
 *     to catch;
 *   - alignment: a frame arrives at whatever offset the read landed on, and a cast to
 *     uint64_t* at an odd address is undefined behaviour - a bus fault on strict
 *     targets and, on x86, silently fine until it is not.
 */
#include "gptps_remote.h"
#include <string.h>

/* --- big-endian primitives ------------------------------------------------ */

static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v);
}
static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)(v);
}
static void put_u64(unsigned char *p, uint64_t v)
{
    p[0] = (unsigned char)(v >> 56); p[1] = (unsigned char)(v >> 48);
    p[2] = (unsigned char)(v >> 40); p[3] = (unsigned char)(v >> 32);
    p[4] = (unsigned char)(v >> 24); p[5] = (unsigned char)(v >> 16);
    p[6] = (unsigned char)(v >> 8);  p[7] = (unsigned char)(v);
}
static uint16_t get_u16(const unsigned char *p)
{ return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]); }
static uint32_t get_u32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint64_t get_u64(const unsigned char *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
}

/* --- status mapping ------------------------------------------------------- */

uint32_t gptps_remote_status_to_wire(gptps_status s)
{
    switch (s) {
        case GPTPS_OK:           return GPTPS_RW_OK;
        case GPTPS_E_INVAL:      return GPTPS_RW_INVAL;
        case GPTPS_E_NOTFOUND:   return GPTPS_RW_NOTFOUND;
        case GPTPS_E_BUDGET:     return GPTPS_RW_BUDGET;
        case GPTPS_E_TIMEOUT:    return GPTPS_RW_TIMEOUT;
        case GPTPS_E_CANCELLED:  return GPTPS_RW_CANCELLED;
        case GPTPS_E_IO:         return GPTPS_RW_IO;
        case GPTPS_E_NOMEM:      return GPTPS_RW_NOMEM;
        case GPTPS_E_DENIED:     return GPTPS_RW_DENIED;
        case GPTPS_E_FULL:       return GPTPS_RW_FULL;
        case GPTPS_E_SHUTDOWN:   return GPTPS_RW_SHUTDOWN;
        default:                 return GPTPS_RW_UNKNOWN;
    }
}

gptps_status gptps_remote_status_from_wire(uint32_t w)
{
    switch (w) {
        case GPTPS_RW_OK:        return GPTPS_OK;
        case GPTPS_RW_INVAL:     return GPTPS_E_INVAL;
        case GPTPS_RW_NOTFOUND:  return GPTPS_E_NOTFOUND;
        case GPTPS_RW_BUDGET:    return GPTPS_E_BUDGET;
        case GPTPS_RW_TIMEOUT:   return GPTPS_E_TIMEOUT;
        case GPTPS_RW_CANCELLED: return GPTPS_E_CANCELLED;
        case GPTPS_RW_IO:        return GPTPS_E_IO;
        case GPTPS_RW_NOMEM:     return GPTPS_E_NOMEM;
        case GPTPS_RW_DENIED:    return GPTPS_E_DENIED;
        case GPTPS_RW_FULL:      return GPTPS_E_FULL;
        case GPTPS_RW_SHUTDOWN:  return GPTPS_E_SHUTDOWN;
        /* A newer peer's code we have never heard of. Degrade to a plain I/O
         * failure: an unknown failure is still a failure, and reinterpreting the
         * number as whatever it happens to mean locally would be worse than useless. */
        default:                 return GPTPS_E_IO;
    }
}

/* --- header codec ---------------------------------------------------------
 * Layout, 24 bytes, big-endian:
 *   [0..3]   magic
 *   [4..5]   version
 *   [6..7]   kind
 *   [8..15]  req_id
 *   [16..19] status
 *   [20..23] payload_len
 */
void gptps_remote_encode_hdr(const gptps_remote_hdr *h, unsigned char *out)
{
    put_u32(out +  0, h->magic);
    put_u16(out +  4, h->version);
    put_u16(out +  6, h->kind);
    put_u64(out +  8, h->req_id);
    put_u32(out + 16, h->status);
    put_u32(out + 20, h->payload_len);
}

gptps_status gptps_remote_decode_hdr(const unsigned char *in, size_t len,
                                     uint32_t max_payload, gptps_remote_hdr *out)
{
    gptps_remote_hdr h;
    if (!in || !out || len < GPTPS_REMOTE_HDR_SIZE) return GPTPS_E_INVAL;
    if (max_payload == 0) max_payload = GPTPS_REMOTE_MAX_PAYLOAD;

    h.magic       = get_u32(in +  0);
    h.version     = get_u16(in +  4);
    h.kind        = get_u16(in +  6);
    h.req_id      = get_u64(in +  8);
    h.status      = get_u32(in + 16);
    h.payload_len = get_u32(in + 20);

    /* Magic first: it is what tells a peer that is not speaking this protocol at all
     * from one that is speaking a version we do not know. */
    if (h.magic != GPTPS_REMOTE_MAGIC)     return GPTPS_E_INVAL;
    if (h.version != GPTPS_REMOTE_VERSION) return GPTPS_E_INVAL;
    switch (h.kind) {
        case GPTPS_REMOTE_REQUEST: case GPTPS_REMOTE_REPLY:
        case GPTPS_REMOTE_CANCEL:  case GPTPS_REMOTE_PING: break;
        default: return GPTPS_E_INVAL;
    }
    /* THE cap, checked here and only here - before the caller has been told how many
     * bytes to read, let alone allocated them. A length is the cheapest thing an
     * unauthenticated peer can lie about. */
    if (h.payload_len > max_payload) return GPTPS_E_INVAL;

    *out = h;
    return GPTPS_OK;
}

void gptps_remote_make_request(gptps_remote_hdr *h, uint64_t req_id, uint32_t payload_len)
{
    h->magic = GPTPS_REMOTE_MAGIC; h->version = GPTPS_REMOTE_VERSION;
    h->kind = GPTPS_REMOTE_REQUEST; h->req_id = req_id;
    h->status = GPTPS_RW_OK; h->payload_len = payload_len;
}

void gptps_remote_make_reply(gptps_remote_hdr *h, uint64_t req_id, gptps_status st,
                             uint32_t payload_len)
{
    h->magic = GPTPS_REMOTE_MAGIC; h->version = GPTPS_REMOTE_VERSION;
    h->kind = GPTPS_REMOTE_REPLY; h->req_id = req_id;
    h->status = gptps_remote_status_to_wire(st); h->payload_len = payload_len;
}

/* --- request payload codec ------------------------------------------------
 * [u32 task_len][task bytes][item bytes]
 *
 * The task name is LENGTH-DELIMITED, not NUL-terminated. A peer must not get to
 * choose where our string ends - and this way a name containing a NUL, or missing
 * one, is a framing error rather than a buffer walk.
 */
gptps_status gptps_remote_encode_request(const char *task,
                                         const void *item, size_t item_len,
                                         unsigned char *buf, size_t *len)
{
    size_t tlen, need;
    if (!task || !buf || !len) return GPTPS_E_INVAL;
    if (item_len && !item)     return GPTPS_E_INVAL;
    tlen = strlen(task);
    /* Bound both parts against uint32 before adding them, so the sum cannot wrap. */
    if (tlen > 0xFFFFFFFFu || item_len > 0xFFFFFFFFu) return GPTPS_E_INVAL;
    need = 4u + tlen + item_len;
    if (need < tlen || need < item_len) return GPTPS_E_INVAL;   /* overflow guard */
    if (*len < need) { *len = need; return GPTPS_E_BUDGET; }

    put_u32(buf, (uint32_t)tlen);
    memcpy(buf + 4, task, tlen);
    if (item_len) memcpy(buf + 4 + tlen, item, item_len);
    *len = need;
    return GPTPS_OK;
}

gptps_status gptps_remote_decode_request(const unsigned char *buf, size_t len,
                                         const char **out_task, size_t *out_task_len,
                                         const void **out_item, size_t *out_item_len)
{
    uint32_t tlen;
    if (!buf || !out_task || !out_task_len || !out_item || !out_item_len) return GPTPS_E_INVAL;
    if (len < 4) return GPTPS_E_INVAL;

    tlen = get_u32(buf);
    /* The obvious attack: a task_len that runs past the frame. Compared against the
     * REMAINING bytes rather than by adding to an offset, so nothing can wrap. */
    if (tlen > len - 4) return GPTPS_E_INVAL;

    *out_task     = (const char *)(buf + 4);
    *out_task_len = tlen;
    *out_item     = (len - 4 - tlen) ? (const void *)(buf + 4 + tlen) : NULL;
    *out_item_len = len - 4 - tlen;
    return GPTPS_OK;
}
