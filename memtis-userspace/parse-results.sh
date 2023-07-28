#!/bin/bash

BENCHMARKS="XSBench graph500 gapbs-pr liblinear silo btree speccpu-bwaves speccpu-roms"
NVM_RATIO="1:2 1:8 1:16"

perf=""
metric=""

for BENCH in ${BENCHMARKS};
do
    echo "----- performance -----" > ${BENCH}-perf.dat
    
    if [[ "x${BENCH}" == "btree" ]]; then
	metric="seconds"
	perf=$(cat results/${BENCH}/all-nvm/static/output.log | grep ${metric} \
	    | awk '{ print $5 }')
    elif [[ "x${BENCH}" =~ "xsilo" ]]; then
	metric="agg_throughput"
	perf=$(cat results/${BENCH}/all-nvm/static/output.log | grep ${metric} \
	    | awk '{ print $2 }')
    else
	metric="execution"
	perf=$(cat results/${BENCH}/all-nvm/static/output.log | grep ${metric} \
	    | awk '{ print $3 }')
    fi
    echo "all-nvm ${perf}" >> ${BENCH}.dat

    for NR in ${NVM_RATIO};
    do
	if [[ "x${BENCH}" == "btree" ]]; then
	    perf=$(cat results/${BENCH}/memtis-all/${NR}/output.log | grep ${metric} \
		| awk '{ print $5 }')
	elif [[ "x${BENCH}" =~ "xsilo" ]]; then
	    perf=$(cat results/${BENCH}/memtis-all/${NR}/output.log | grep ${metric} \
		| awk '{ print $2 }')
	else
	    perf=$(cat results/${BENCH}/memtis-all/${NR}/output.log | grep ${metric} \
		| awk '{ print $3 }')
	fi
	echo "${NR} ${perf}" >> ${BENCH}.dat
    done
done
