#!/bin/bash

######## changes the below path
BIN=/home/taehyung/workspace/tmm/memtis-userspace/bench_dir/gapbs
GRAPH_DIR=/home/taehyung/workspace/tmm/memtis-userspace/bench_dir/gapbs/benchmark/graphs


BENCH_RUN="${BIN}/pr -f ${GRAPH_DIR}/twitter.sg -i1000 -t1e-4 -n16"
BENCH_DRAM=""


# twitter.sg: ~12600MB 

if [[ "x${NVM_RATIO}" == "x1:32" ]]; then
    BENCH_DRAM="382MB"
elif [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="740MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="1400MB" # twitter.sg
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="2520MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="4200MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="6300MB"
elif [[ "x${NVM_RATIO}" == "x2:1" ]]; then
    BENCH_DRAM="8400MB"
elif [[ "x${NVM_RATIO}" == "x4:1" ]]; then
    BENCH_DRAM="10080MB"
elif [[ "x${NVM_RATIO}" == "x8:1" ]]; then
    BENCH_DRAM="11200MB"
elif [[ "x${NVM_RATIO}" == "x16:1" ]]; then
    BENCH_DRAM="11859MB"
elif [[ "x${NVM_RATIO}" == "x1:0" ]]; then
    BENCH_DRAM="70000MB"
fi


export BENCH_RUN
export BENCH_DRAM
