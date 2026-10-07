# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Fikoko. See LICENSE for the full text.
#
# smoke_test.cmake - CTest's example_edge_ai: edge_admission in GPTPS mode on a tiny
# night, under a second in all, and checks on what it reports:
#   - every normal job finishes, on its first attempt;
#   - the flaky job finishes on its second;
#   - the runaway is stopped by its cap, and ends as a dead letter;
#   - a job that can never fit the budget is refused at submit;
#   - the declared memory in flight never passed the budget;
#   - each job that ran reports what GPTPS measured at its peak (docs/MEASUREMENTS.md),
#     the runaway well past what a normal job takes.
# The cap is RLIMIT_AS: GPTPS_CGROUP_PARENT is cleared, so the test needs neither
# cgroups nor Docker.
#
#   cmake -DEDGE_ADMISSION=<path> -DFAKE_INFER_DIR=<dir> -DWORK_DIR=<dir> -P smoke_test.cmake

foreach(v EDGE_ADMISSION FAKE_INFER_DIR WORK_DIR)
    if(NOT ${v})
        message(FATAL_ERROR "smoke_test.cmake needs -D${v}=...")
    endif()
endforeach()

# 160 MB each against a 400 MB budget: two jobs at a time at most. A job takes 128
# MB: well above what edge_admission itself holds - which on Linux is what lets a
# program's own peak be told from the copy of the host it was forked as (docs/
# MEASUREMENTS.md) - even under a sanitizer, whose host can hold 50 MB once huge
# pages back its allocator. The runaway keeps its pace, about 0.25 ms per MB, past its
# 128 and is refused at its 160 MB cap.
set(jobs "${WORK_DIR}/edge_ai_smoke_jobs.txt")
set(mark "${WORK_DIR}/edge_ai_smoke.mark")
file(WRITE "${jobs}" "\
# name   mem_mb gpu timeout_s retries command
a          160   1       10       0  fake_infer --mb 128 --ms 120
b          160   1       10       0  fake_infer --mb 128 --ms 120
c          160   1       10       0  fake_infer --mb 128 --ms 120
flaky      160   1       10       1  fake_infer --mb 128 --ms 120 --fail-first ${mark}
runaway    160   1       10       1  fake_infer --mb 128 --ms 120 --runaway
too_big    500   1       10       0  fake_infer --mb 128 --ms 120
")
file(REMOVE "${mark}")
set(ENV{PATH} "${FAKE_INFER_DIR}:$ENV{PATH}")
unset(ENV{GPTPS_CGROUP_PARENT})

execute_process(COMMAND "${EDGE_ADMISSION}" --budget-mb 400 --gpu-slots 2 "${jobs}"
                WORKING_DIRECTORY "${WORK_DIR}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")

set(bad 0)
function(expect what regex var)          # var: the NAME of the variable to search
    if(NOT "${${var}}" MATCHES "${regex}")
        message(SEND_ERROR "example_edge_ai: expected ${what}")
        math(EXPR n "${bad} + 1")
        set(bad ${n} PARENT_SCOPE)
    endif()
endfunction()

expect("exit status 1 (the runaway and too_big fail for good)" "^1$" rc)
# The peak column prints with one decimal ("49.6"), so it cannot pass for the gpu
# column of a table printed without it.
foreach(j a b c)
    expect("${j} to finish on its first attempt" "\n${j} +160 +[0-9]+\\.[0-9] +1 +[0-9]+ +[0-9]+ +1  finished\n" out)
endforeach()
expect("flaky to finish on its second attempt"
       "\nflaky +160 +[0-9]+\\.[0-9] +1 +[0-9]+ +[0-9]+ +2  finished after a retry\n" out)
expect("runaway to be a dead letter after 2 attempts"
       "\nrunaway +160 +[0-9]+\\.[0-9] +1 +[0-9]+ +[0-9]+ +2  dead letter: GPTPS_E_TASK\n" out)
expect("the runaway's allocation to be refused by its cap"
       "fake_infer: --runaway: allocation refused at" err)
expect("too_big to be refused at submit"
       "\ntoo_big +500 +- +1 +- +- +0  refused: GPTPS_E_BUDGET" out)
expect("the declared peak in flight to be reported"
       "peak declared in flight: [0-9]+ MB of a 400 MB budget, gpu [0-9]+ of 2" out)
if("${out}" MATCHES "peak declared in flight: ([0-9]+) MB of a 400 MB budget, gpu ([0-9]+) of 2")
    if(CMAKE_MATCH_1 GREATER 400 OR CMAKE_MATCH_2 GREATER 2)
        message(SEND_ERROR "example_edge_ai: in flight ${CMAKE_MATCH_1} MB, gpu ${CMAKE_MATCH_2}: over the budget")
        math(EXPR bad "${bad} + 1")
    endif()
endif()
# Measured peaks: a normal job took 128 MB, the runaway kept going until its 160 MB
# cap refused it - so its peak is the larger, and both are real numbers.
expect("the measured peaks to be reported" "peak MB: measured by GPTPS" out)
if("${out}" MATCHES "\na +160 +([0-9]+\\.[0-9]) " AND NOT "${CMAKE_MATCH_1}" LESS 128)
    set(a_peak "${CMAKE_MATCH_1}")
else()
    message(SEND_ERROR "example_edge_ai: job a's measured peak is missing or under the 128 MB it took")
    math(EXPR bad "${bad} + 1")
endif()
if("${out}" MATCHES "\nrunaway +160 +([0-9]+\\.[0-9]) ")
    if(DEFINED a_peak AND NOT "${CMAKE_MATCH_1}" GREATER "${a_peak}")
        message(SEND_ERROR "example_edge_ai: the runaway's peak ${CMAKE_MATCH_1} MB is not above job a's ${a_peak} MB")
        math(EXPR bad "${bad} + 1")
    endif()
else()
    message(SEND_ERROR "example_edge_ai: the runaway's measured peak is missing")
    math(EXPR bad "${bad} + 1")
endif()
if(EXISTS "${mark}")
    message(SEND_ERROR "example_edge_ai: the flaky job's retry should have removed ${mark}")
    math(EXPR bad "${bad} + 1")
endif()

if(bad)
    message(FATAL_ERROR "example_edge_ai: ${bad} check(s) failed")
endif()
message("example_edge_ai: all checks passed")
