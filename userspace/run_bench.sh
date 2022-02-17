#!/bin/bash

if [ -z $NTHREADS ]; then
    NTHREADS=$(grep -c processor /proc/cpuinfo)
fi
export NTHREADS
NCPU_NODES=$(cat /sys/devices/system/node/has_cpu | awk -F '-' '{print $NF+1}')
NMEM_NODES=$(cat /sys/devices/system/node/has_memory | awk -F '-' '{print $NF+1}')
MEM_NODES=($(ls /sys/devices/system/node | grep node | awk -F 'node' '{print $NF}'))

CGROUP_NAME="htmm"

CONFIG_PERF=off
CONFIG_DATE=no
CONFIG_MAX_THREADS=no

DATE=""
DRAM_SIZE=""

function func_cache_flush() {
    echo 3 > /proc/sys/vm/drop_caches
    free
    return
}

function func_prepare() {
    echo "Preparing benchmark start..."

	#if [[ "x${CONFIG_PERF}" = "xoff" ]]; then
	#    sudo sysctl kernel.perf_event_max_sample_rate=1
	#else
	sudo sysctl kernel.perf_event_max_sample_rate=100000
	#fi

	# disable automatic numa balancing
	sudo echo 0 > /proc/sys/kernel/numa_balancing

	# check date
	if [[ "x${CONFIG_DATE}" == "xno" ]]; then
	    DATE=$(date +%Y%m%d%H%M)
	fi

	if [[ "x${CONFIG_MAX_THREADS}" == "xyes" ]]; then
	    NTHREADS=${MAX_THREADS}
	    export NTHREADS
	fi

	export BENCH_NAME
	export BENCH_SIZE

	if [[ -e ./benches/${BENCH_NAME}.sh ]]; then
	    source ./benches/${BENCH_NAME}.sh
	else
	    echo "ERROR: ${BENCH_NAME}.sh does not exist."
	    exit -1
	fi
}

function func_main() {

    TEST_NAME=""
    TIME="/usr/bin/time"

    if [[ "x${CONFIG_PERF}" == "xon" ]]; then
	PERF="./perf stat -e dtlb_store_misses.walk_pending,dtlb_load_misses.walk_pending,dTLB-store-misses,cycle_activity.stalls_total"
    else
	PERF=""
    fi
    
    # use 15 threads 
    PINNING="taskset -c 0-15"
    #PINNING="numactl -N 0"

    # get FREE DRAM SIZE of node 0 (in mb)
    MAX_DRAM_SIZE=$(numastat -m | awk '$1 == "MemFree" { print int($2) }')
    let MAX_DRAM_SIZE=${MAX_DRAM_SIZE}/1024

    if [[ "xDRAM_SIZE" == "xMAX" ]]; then
	DRAM_SIZE="${MAX_DRAM_SIZE}GB"
    else
	if [[ ${DRAM_SIZE::-2} -gt ${MAX_DRAM_SIZE} ]]; then
	    DRAM_SIZE="${MAX_DRAM_SIZE}GB"
	fi
    fi

    # make directory for results
    mkdir -p results/${BENCH_NAME}/${DRAM_SIZE}
    LOG_DIR=results/${BENCH_NAME}/${DRAM_SIZE}

    # set memcg for htmm
    sudo ./set_htmm_memcg.sh htmm $$ enable
    sudo ./set_mem_size.sh htmm 0 ${DRAM_SIZE}
    sleep 2

    # flush cache
    func_cache_flush
    sleep 2

    ${TIME} -f "execution time %e (s)" \
	${PINNING} ./launch_bench ${BENCH_RUN} 2>&1 \
	| tee ${LOG_DIR}/output.log
    
    sleep 2
    # disable htmm
    sudo ./set_htmm_memcg.sh htmm $$ disable
}

function func_usage() {
    echo
    echo -e "Usage: $0 [-b benchmark name] [-s socket_mode] [-w GB] ..."
    echo
    echo "  -B, --benchmark   [arg]    benchmark name to run. e.g., graph500, Liblinear, etc"
    echo "  -S, --socket      [arg]    select socket mode: \"single\" or \"multi\""
    echo "  -W, --wss         [arg]    working set size with \"-GB\" postfix"
    echo "      --max-threads [arg]    maximum threads"
    echo "  -d. --date        [arg]    benchmark start time"
    echo "  -D, --dramsize    [arg]    dram size"
    echo "  -?, --help                 give this help list"
    echo "      --usage"
    echo
}


################################ Main ##################################

if [ "$#" == 0 ]; then
    echo "Error: no arguments"
    func_usage
    exit -1
fi

# get options:
while (( "$#" )); do
    case "$1" in
	-B|--benchmark)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		BENCH_NAME=( "$2" )
		shift 2
	    else
		echo "Error: Argument for $1 is missing" >&2
		func_usage
		exit -1
	    fi
	    ;;
	-P|--perf)
	    CONFIG_PERF=on
	    shift 1
	    ;;
	-W|--wss)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		BENCH_SIZE="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	--max-threads)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		CONFIG_MAX_THREADS=yes
		MAX_THREADS="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-d|--date)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		CONFIG_DATE=yes
		DATE="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-D|--dramsize)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		DRAM_SIZE="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-H|-?|-h|--help|--usage)
	    func_usage
	    exit
	    ;;
	*)
	    echo "Error: Invalid option $1"
	    func_usage
	    exit -1
	    ;;
    esac
done

if [ -z "${BENCH_NAME}" ]; then
    echo "Benchmark name must be specified"
    func_usage
    exit -1
fi

func_prepare
func_main
