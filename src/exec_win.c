/*
 * exec_win.c - Windows executor backend (counterpart to exec_oop_posix.c).
 *
 * The OOP executor (run an in-process task function inside an isolated, killable
 * CHILD) is built on POSIX fork() + cgroups/RLIMIT, which Windows has no direct
 * equivalent for. The external-PROGRAM executor (CreateProcess + a Job Object for
 * the memory cap and a single TerminateJobObject kill) is the natural Win32
 * mapping and is the planned next increment.
 *
 * Until then these report GPTPS_E_INVAL so the engine LINKS and EXEC_INPROC works
 * fully on Windows; EXEC_OOP / EXEC_PROGRAM tasks fail cleanly (and visibly) here.
 */
#if defined(_WIN32)

#include "gptps.h"
#include "gptps_internal.h"

gptps_status gptps_oop_execute(const gptps_task_def *def, const void *payload, size_t plen,
                               uint64_t mem_cap, uint32_t timeout_s,
                               void **out_result, size_t *out_len)
{
    (void)def; (void)payload; (void)plen; (void)mem_cap; (void)timeout_s;
    *out_result = NULL; *out_len = 0;
    return GPTPS_E_INVAL; /* fork-based isolation is POSIX-only */
}

gptps_status gptps_program_execute(const char *const *argv, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s,
                                   void **out_result, size_t *out_len)
{
    (void)argv; (void)payload; (void)plen; (void)mem_cap; (void)timeout_s;
    *out_result = NULL; *out_len = 0;
    return GPTPS_E_INVAL; /* TODO: CreateProcess + Job Object on Windows */
}

#endif /* _WIN32 */
