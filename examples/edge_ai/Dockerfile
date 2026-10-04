# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Fikoko. See LICENSE for the full text.
#
# The edge-AI demo in a container. run_demo.sh builds it from the repository root
# and runs it with --memory and --memory-swap set to the board's size, so that the
# container plays the board and takes the naive run's OOM kills:
#
#   docker build -f examples/edge_ai/Dockerfile -t gptps-edge-ai .
#   docker run --rm --init --network none --memory=1g --memory-swap=1g gptps-edge-ai
#
# GPTPS goes in as its two-file amalgamation (tools/amalgamate.sh), the way a host
# embeds it, with no build system. Both images are Debian bookworm and exist for
# amd64 and arm64, so this builds on a Jetson as well. For jobs that use the GPU in
# a container on a Jetson, NVIDIA's base is nvcr.io/nvidia/l4t-jetpack, with the
# tag that matches the board's L4T release; pass it as both BUILD_IMAGE and
# RUN_IMAGE. That combination is not tested here (see README.md).
ARG BUILD_IMAGE=gcc:13-bookworm
ARG RUN_IMAGE=debian:bookworm-slim

FROM ${BUILD_IMAGE} AS build
WORKDIR /src
COPY LICENSE ./
COPY include include
COPY src src
COPY tools/amalgamate.sh tools/
COPY examples/edge_ai examples/edge_ai
RUN mkdir /out \
 && sh tools/amalgamate.sh /amalg \
 && cc -std=c99 -O2 -Wall -Wextra -I/amalg /amalg/gptps.c examples/edge_ai/edge_admission.c \
       -lpthread -ldl -o /out/edge_admission \
 && cc -std=c99 -O2 -Wall -Wextra examples/edge_ai/fake_infer.c -o /out/fake_infer

FROM ${RUN_IMAGE}
COPY --from=build /out/ /usr/local/bin/
COPY examples/edge_ai/jobs.txt /demo/jobs.txt
WORKDIR /demo
CMD ["edge_admission", "--budget-mb", "768", "--gpu-slots", "4", "jobs.txt"]
