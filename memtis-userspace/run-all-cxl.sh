#!/bin/bash

BENCHMARKS="XSBench graph500 gapbs-pr liblinear silo btree speccpu-bwaves speccpu-roms"

sudo dmesg -c

# enable THP
sudo echo "always" | tee /sys/kernel/mm/transparent_hugepage/enabled
sudo echo "always" | tee /sys/kernel/mm/transparent_hugepage/defrag
# decrease uncore freq
sudo ./scripts/set_uncore_freq.sh on

for BENCH in ${BENCHMARKS};
do
    export GOMP_CPU_AFFINITY=0-19
    if [[ -e ./benches/${BENCH}.sh ]]; then
	source ./bench_cmds/${BENCH}.sh
    else
	echo "ERROR: ${BENCH}.sh does not exist."
	continue
    fi

    mkdir -p results/${BENCH}/all-nvm/static
    LOG_DIR=results/${BENCH}/all-nvm/static

    free;sync;echo 3 > /proc/sys/vm/drop_caches;free;

    time -f "execution time %e (s)" \
	numactl -N 0 -m 1 ${BENCH_RUN} 2>&1 \
	| tee ${LOG_DIR}/output.log

done

sudo ./scripts/set_uncore_freq.sh off
