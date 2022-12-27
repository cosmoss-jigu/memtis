#!/bin/bash
BENCH_BIN=/home/taehyung/workspace/tmm/memtis-userspace/bench_dir/liblinear-multicore-2.45-1

MAX_THREADS=$(grep -c processor /proc/cpuinfo)
BENCH_RUN=""
BENCH_DRAM=""

# anon footprint 79640MB
# file footprint 21581MB

BENCH_RUN+="${BENCH_BIN}/train -s 6 -m 16 ${BENCH_BIN}/datasets/kdd12"


if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="4150MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    #B-ENCH_DRAM="7849MB"
    BENCH_DRAM="8000MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="14128MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="23000MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="35320MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="80000MB"
fi


export BENCH_RUN
export BENCH_DRAM
