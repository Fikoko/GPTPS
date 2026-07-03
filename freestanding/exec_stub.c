/*
 * exec_stub.c - freestanding stubs for the out-of-process executors.
 *
 * GPTPS_EXEC_OOP (fork) and GPTPS_EXEC_PROGRAM (fork+exec) need an OS process
 * model that does not exist on bare metal, so a freestanding build provides
 * these as unavailable stubs. The freestanding demo uses GPTPS_EXEC_INPROC only;
 * the engine references these symbols unconditionally, so they exist to link.
 */
#include "gptps_internal.h"

gptps_status gptps_oop_execute(const gptps_task_def *def, const void *payload, size_t plen,
                               uint64_t mem_cap, uint32_t timeout_s, gptps_flag *cancel,
                               void **out_result, size_t *out_len)
{
    (void)def; (void)payload; (void)plen; (void)mem_cap; (void)timeout_s; (void)cancel;
    if (out_result) *out_result = 0;
    if (out_len) *out_len = 0;
    return GPTPS_E_IO;   /* no process model in a freestanding build */
}

gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s, gptps_flag *cancel,
                                   void **out_result, size_t *out_len)
{
    (void)def; (void)payload; (void)plen; (void)mem_cap; (void)timeout_s; (void)cancel;
    if (out_result) *out_result = 0;
    if (out_len) *out_len = 0;
    return GPTPS_E_IO;
}
