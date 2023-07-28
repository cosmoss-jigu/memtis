#!/bin/bash

BENCHMARKS="XSBench graph500 gapbs-pr liblinear silo btree speccpu-bwaves speccpu-roms"
#BENCHMARKS="gapbs-pr"
NVM_RATIO="1:8"

sudo dmesg -c

for BENCH in ${BENCHMARKS};
do
    for NR in ${NVM_RATIO};
    do
	./scripts/run_bench.sh -B ${BENCH} -R ${NR} -NW -NS -V memtis-vanila
	sleep 10
	./scripts/run_bench.sh -B ${BENCH} -R ${NR} -NW -V memtis-split
	sleep 10
	./scripts/run_bench.sh -B ${BENCH} -R ${NR} -NS -V memtis-warm
    done
done
