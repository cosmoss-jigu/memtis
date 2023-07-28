#!/bin/bash

BENCHMARKS="XSBench graph500 gapbs-pr liblinear silo btree speccpu-bwaves speccpu-roms"
NVM_RATIO="1:2 1:8 1:16"

perf=""
metric=""

####### Fig. 5
echo "----- performance -----" > memtis-perf.dat
for BENCH in ${BENCHMARKS};
do
    echo "[[[[[ ${BENCH} ]]]]]" >> memtis-perf.dat
    
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
    echo "all-nvm ${perf}" >> memtis-perf.dat

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
	echo "${NR} ${perf}" >> memtis-perf.dat
    done
    echo "" >> memtis-perf.dat
done


####### Fig.7
echo "----- performance -----" > memtis-scalability.dat
BENCHMARKS="graph500-128GB graph500-192GB graph500-336GB graph500-690GB"
for BENCH in ${BENCHMARKS};
do
    echo "[[[[[ ${BENCH} ]]]]]" >> memtis-scalability.dat
    
    metric="execution"
    perf=$(cat results/${BENCH}/all-nvm/static/output.log | grep ${metric} \
        | awk '{ print $3 }')
    echo "all-nvm ${perf}" >> memtis-scalability.dat

    perf=$(cat results/${BENCH}/memtis-scalability/static/output.log | grep ${metric} \
        | awk '{ print $3 }')
    echo "memtis ${perf}" >> memtis-scalability.dat
    echo "" >> memtis-scalability.dat
done


####### Fig. 8
VERSION="memtis-vanila memtis-split memtis-warm memtis-all"
BENCHMARKS="XSBench graph500 gapbs-pr liblinear silo btree speccpu-bwaves speccpu-roms"

echo "----- stat -----" > memtis-stat.dat
for BENCH in ${BENCHMARKS};
do
    echo "[[[[[ ${BENCH} ]]]]]" >> memtis-stat.dat
    NR="1:8"
    metric="pgmigrate_su"

    for V in ${VERSION};
    do
	before=$(cat results/${BENCH}/${V}/${NR}/before_vmstat.log | grep ${metric} \
	    | awk '{ print $2 }')
	after=$(cat results/${BENCH}/${V}/${NR}/after_vmstat.log | grep ${metric} \
	    | awk '{ print $2 }')
	let after=after-before
	echo "${NR} ${V} ${after}" >> memtis-stat.dat
    done
    echo "" >> memtis-stat.dat
done


####### Fig. 10
echo "----- hit ratio -----" > memtis-hitratio.dat
for BENCH in ${BENCHMARKS};
do
    echo "[[[[[ ${BENCH} ]]]]]" >> memtis-hitratio.dat
    NR="1:8"
    metric="hits"

    total=$(cat results/${BENCH}/memtis-all/${NR}/dmesg.txt | grep ${metric} | tail -n 1 \
	| awk '{ print $4 }')
    max_hits=$(cat results/${BENCH}/memtis-all/${NR}/dmesg.txt | grep ${metric} | tail -n 1 \
	| awk '{ print $6 }')
    cur_hits=$(cat results/${BENCH}/memtis-all/${NR}/dmesg.txt | grep ${metric} | tail -n 1 \
	| awk '{ print $8 }')

    eHR=$(bc<<<"scale=2; ${max_hits}*100/${total}")
    rHR=$(bc<<<"scale=2; ${cur_hits}*100/${total}")

    total=$(cat results/${BENCH}/memtis-warm/${NR}/dmesg.txt | grep ${metric} | tail -n 1 \
	| awk '{ print $4 }')
    cur_hits=$(cat results/${BENCH}/memtis-warm/${NR}/dmesg.txt | grep ${metric} | tail -n 1 \
	| awk '{ print $8 }')

    rHR_NS=$(bc<<<"scale=2; ${cur_hits}*100/${total}")

    echo "eHR(%): ${eHR} | rHR-NS(%): ${rHR_NS} | rHR(%): ${rHR}" >> memtis-hitratio.dat
    echo "" >> memtis-hitratio.dat
done


####### Fig. 11
echo "----- performance -----" > memtis-cxl.dat
for BENCH in ${BENCHMARKS};
do
    echo "[[[[[ ${BENCH} ]]]]]" >> memtis-cxl.dat
    
    if [[ "x${BENCH}" == "btree" ]]; then
	metric="seconds"
	perf=$(cat results/${BENCH}/all-cxl/static/output.log | grep ${metric} \
	    | awk '{ print $5 }')
    elif [[ "x${BENCH}" =~ "xsilo" ]]; then
	metric="agg_throughput"
	perf=$(cat results/${BENCH}/all-cxl/static/output.log | grep ${metric} \
	    | awk '{ print $2 }')
    else
	metric="execution"
	perf=$(cat results/${BENCH}/all-cxl/static/output.log | grep ${metric} \
	    | awk '{ print $3 }')
    fi
    echo "all-cxl ${perf}" >> memtis-cxl.dat

    for NR in ${NVM_RATIO};
    do
	if [[ "x${BENCH}" == "btree" ]]; then
	    perf=$(cat results/${BENCH}/memtis-cxl/${NR}/output.log | grep ${metric} \
		| awk '{ print $5 }')
	elif [[ "x${BENCH}" =~ "xsilo" ]]; then
	    perf=$(cat results/${BENCH}/memtis-cxl/${NR}/output.log | grep ${metric} \
		| awk '{ print $2 }')
	else
	    perf=$(cat results/${BENCH}/memtis-cxl/${NR}/output.log | grep ${metric} \
		| awk '{ print $3 }')
	fi
	echo "${NR} ${perf}" >> memtis-cxl.dat
    done
    echo "" >> memtis-cxl.dat
done

