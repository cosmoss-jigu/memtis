#!/bin/bash

BENCHMARKS="graph500-128GB graph500-192GB graph500-336GB graph500-690GB"

sudo dmesg -c

# enable THP
sudo echo "always" | tee /sys/kernel/mm/transparent_hugepage/enabled
sudo echo "always" | tee /sys/kernel/mm/transparent_hugepage/defrag

for BENCH in ${BENCHMARKS};
do
    export GOMP_CPU_AFFINITY=0-19
    if [[ -e ./bench_cmds/${BENCH}.sh ]]; then
	source ./bench_cmds/${BENCH}.sh
    else
	echo "ERROR: ${BENCH}.sh does not exist."
	continue
    fi

    mkdir -p results/${BENCH}/all-nvm/static
    LOG_DIR=results/${BENCH}/all-nvm/static

    free;sync;echo 3 > /proc/sys/vm/drop_caches;free;

    /usr/bin/time -f "execution time %e (s)" \
	numactl -N 0 -m 2 ${BENCH_RUN} 2>&1 \
	| tee ${LOG_DIR}/output.log

done
