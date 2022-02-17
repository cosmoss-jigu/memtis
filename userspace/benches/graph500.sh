#!/bin/bash

BIN=/home/taehyung/workspace/tmm/userspace/bench_bin
BENCH_RUN=""

if [[ "x${BENCH_SIZE}" == "x64GB" ]]; then
    BENCH_RUN+="${BIN}/graph500 -s 27 -e 15 -V"
elif [[ "x${BENCH_SIZE}" == "x96GB" ]]; then
    BENCH_RUN+="${BIN}/graph500 -s 27 -e 23 -V"
elif [[ "x${BENCH_SIZE}" == "x128GB" ]]; then
    BENCH_RUN+="${BIN}/graph500 -s 28 -e 15 -V"
fi

export BENCH_RUN
