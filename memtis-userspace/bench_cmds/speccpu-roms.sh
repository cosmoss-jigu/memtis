#!/bin/bash

BIN=/path/to/benchmark/benchspec/CPU/654.roms_s/run/run_base_refspeed_mytest-m64-test.0000
BENCH_RUN="${BIN}/sroms_base.mytest-m64-test"
BENCH_ARG="${BIN}/ocean_benchmark3.in"
BENCH_DRAM=""


#####
# orig: 11000MB
# bwaves:: 64GB memory footprint
#####

if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="647MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="1222MB"
elif [[ "x${NVM_RATIO}" == "x1:4" ]]; then
    BENCH_DRAM="2200MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="3900MB"
elif [[ "x${NVM_RATIO}" == "x1:1" ]]; then
    BENCH_DRAM="5500MB"
elif [[ "x${NVM_RATIO}" == "x2:1" ]]; then
    BENCH_DRAM="7330MB"
elif [[ "x${NVM_RATIO}" == "x3:1" ]]; then
    BENCH_DRAM="8250MB"
fi

export BENCH_ARG
export BENCH_RUN
export BENCH_DRAM
