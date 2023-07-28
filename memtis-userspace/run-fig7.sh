#!/bin/bash

BENCHMARKS="graph500-128GB graph500-192GB graph500-336GB graph500-690GB"
#BENCHMARKS="gapbs-pr"
NVM_RATIO="static"
STATIC_DRAM="64GB"

sudo dmesg -c

for BENCH in ${BENCHMARKS};
do
    for NR in ${NVM_RATIO};
    do
	./scripts/run_bench.sh -B ${BENCH} -R ${NR} -D ${STATIC_DRAM} -V memtis-scalability
    done
done
