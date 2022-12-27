#!/bin/bash

BENCHMARKS="XSBench gapbs-pr liblinear silo btree"
BENCHMARKS="gapbs-pr"
NVM_RATIO="1:16 1:8 1:2"

sudo dmesg -c

for BENCH in ${BENCHMARKS};
do
    for NR in ${NVM_RATIO};
    do
	./scripts/run_bench.sh -B ${BENCH} -R ${NR} -V test
    done
done
