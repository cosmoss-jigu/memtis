#!/bin/bash

BIN=/path/to/benchmark
BENCH_RUN="${BIN}/bench_btree_mt"
BENCH_DRAM=""


if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="2350MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="4500MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="7870MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="13100MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="19675MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="75000MB"
fi


export BENCH_RUN
export BENCH_DRAM
