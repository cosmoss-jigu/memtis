#!/bin/bash

BIN=/path/to/benchmark
BENCH_RUN="${BIN}/graph500 -s 29 -e 20 -V"
BENCH_DRAM=""

if [[ "x${NVM_RATIO}" == "xstatic" ]]; then
    if [[ ${STATIC_DRAM} =~ "MB" ]]; then
	BENCH_DRAM=${STATIC_DRAM}
    elif [[ ${STATIC_DRAM} =~ "GB" ]]; then
	BENCH_DRAM=`echo ${STATIC_DRAM::-2}*1024 | bc`
	BENCH_DRAM=${BENCH_DRAM}MB
    fi
fi

export BENCH_RUN
export BENCH_DRAM
