#!/bin/bash
BENCH_BIN=/path/to/benchmark

# anon footprint 79640MB
# file footprint 21581MB

BENCH_RUN="${BENCH_BIN}/train -s 6 -m 20 ${BENCH_BIN}/datasets/kdd12"
# Liblinear requires a dataset file (kdd12)
# Please refer to memtis-userspace/bench_dir/README.md for downloading this dataset

if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="4150MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
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
