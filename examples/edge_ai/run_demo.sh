#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Fikoko. See LICENSE for the full text.
#
# run_demo.sh - build the edge-AI demo and run it twice in a container whose memory
# is the board's: once with GPTPS admitting the jobs, once with every job started at
# once (--naive). The naive run is meant to run out of memory; the container's limit
# takes its OOM kills, not this machine.
#
#   examples/edge_ai/run_demo.sh                    a 1 GB board, the synthetic night
#   examples/edge_ai/run_demo.sh --board-mb 2048    a 2 GB board, budget 1536 MB
#   examples/edge_ai/run_demo.sh --host             no Docker: GPTPS mode, on this machine
#
#   --board-mb N    the board's memory: the container's --memory and --memory-swap
#                   (default 1024)
#   --budget-mb N   what the jobs may declare in flight at once, limits.max_memory_bytes
#                   (default 3/4 of the board)
#   --gpu-slots N   the budget of the resource "gpu" (default 4)
#   --jobs FILE     the jobs file (default examples/edge_ai/jobs.txt)
#   --host          build with CMake and run GPTPS mode on this machine, without Docker.
#                   Safe: GPTPS keeps what the jobs declare in flight within
#                   --budget-mb, and caps each job at its declaration.
#   --unsafe-host   with --host, run naive mode on this machine as well. Nothing limits
#                   it: it can drive this machine into its own OOM killer.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BOARD_MB=1024
BUDGET_MB=
GPU_SLOTS=4
JOBS="$HERE/jobs.txt"
HOST=0
UNSAFE=0
IMAGE=gptps-edge-ai

usage() { sed -n '/^#   examples/,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2; }
while [ $# -gt 0 ]; do
    case "$1" in
        --board-mb)    BOARD_MB=${2:?}; shift 2 ;;
        --budget-mb)   BUDGET_MB=${2:?}; shift 2 ;;
        --gpu-slots)   GPU_SLOTS=${2:?}; shift 2 ;;
        --jobs)        JOBS=${2:?}; shift 2 ;;
        --host)        HOST=1; shift ;;
        --unsafe-host) HOST=1; UNSAFE=1; shift ;;
        *)             usage ;;
    esac
done
BUDGET_MB=${BUDGET_MB:-$((BOARD_MB * 3 / 4))}
JOBS=$(CDPATH= cd -- "$(dirname -- "$JOBS")" && pwd)/$(basename -- "$JOBS")
[ -r "$JOBS" ] || { echo "run_demo.sh: cannot read $JOBS" >&2; exit 2; }
if [ "$BUDGET_MB" -ge "$BOARD_MB" ]; then
    echo "run_demo.sh: --budget-mb ($BUDGET_MB) must be less than --board-mb ($BOARD_MB): the rest of the board needs memory too" >&2
    exit 2
fi

# edge_admission exits 1 when a job fails for good, which the synthetic night does
# on purpose (the runaway), and 2 on a setup error. Only a 2 or worse stops us.
show() {
    rc=0
    "$@" || rc=$?
    echo "(edge_admission exited $rc)"
    [ "$rc" -le 1 ] || exit "$rc"
}

run_host() {
    BUILD="$ROOT/build-edge-ai"
    echo "== building edge_admission and fake_infer in $BUILD"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DGPTPS_BUILD_TESTS=OFF >/dev/null
    cmake --build "$BUILD" --target edge_admission fake_infer >/dev/null
    echo
    echo "== GPTPS mode, on this machine: budget ${BUDGET_MB} MB, gpu ${GPU_SLOTS} slots"
    show env PATH="$BUILD:$PATH" "$BUILD/edge_admission" \
        --budget-mb "$BUDGET_MB" --gpu-slots "$GPU_SLOTS" "$JOBS"
    if [ "$UNSAFE" = 1 ]; then
        echo
        echo "== naive mode, on this machine, with nothing to limit it (--unsafe-host)"
        show env PATH="$BUILD:$PATH" "$BUILD/edge_admission" --naive --budget-mb "$BUDGET_MB" "$JOBS"
    else
        echo
        echo "Naive mode was not run: outside a memory-limited container nothing stops it"
        echo "from taking this machine's memory. Use Docker, or --unsafe-host if you mean it."
    fi
}

if [ "$HOST" = 1 ]; then
    run_host
    exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
    why="Docker is not installed"
elif ! docker info >/dev/null 2>&1; then
    why="docker is installed, but its daemon does not answer (docker info failed)"
else
    why=
fi
if [ -n "$why" ]; then
    echo "run_demo.sh: $why."
    echo "Naive mode needs a memory-limited container and will not run without one."
    echo "GPTPS mode is safe on this machine: what its jobs declare in flight stays"
    echo "within --budget-mb ($BUDGET_MB MB), and each job is capped at its declaration."
    if [ -t 0 ]; then
        printf 'Run GPTPS mode here instead? [y/N] '
        read -r answer || answer=
        case "$answer" in
            y|Y|yes) run_host; exit 0 ;;
        esac
    else
        echo "To run it: $0 --host"
    fi
    exit 1
fi

echo "== building the $IMAGE image (the first time pulls two Debian images)"
docker build -q --network none -f "$HERE/Dockerfile" -t "$IMAGE" "$ROOT" >/dev/null

# The container is the board: BOARD_MB of memory, no swap, nothing else running.
board() {
    docker run --rm --init --network none \
        --memory="${BOARD_MB}m" --memory-swap="${BOARD_MB}m" \
        -v "$JOBS:/demo/jobs.txt:ro" "$IMAGE" "$@"
}

echo
echo "== GPTPS mode, on a ${BOARD_MB} MB board: budget ${BUDGET_MB} MB, gpu ${GPU_SLOTS} slots"
show board edge_admission --budget-mb "$BUDGET_MB" --gpu-slots "$GPU_SLOTS" jobs.txt
echo
echo "== naive mode, on a ${BOARD_MB} MB board: every job at once"
show board edge_admission --naive --budget-mb "$BUDGET_MB" jobs.txt
