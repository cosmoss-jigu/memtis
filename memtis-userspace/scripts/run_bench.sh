#!/bin/bash

if [ -z $NTHREADS ]; then
    NTHREADS=$(grep -c processor /proc/cpuinfo)
fi
export NTHREADS
NCPU_NODES=$(cat /sys/devices/system/node/has_cpu | awk -F '-' '{print $NF+1}')
NMEM_NODES=$(cat /sys/devices/system/node/has_memory | awk -F '-' '{print $NF+1}')
MEM_NODES=($(ls /sys/devices/system/node | grep node | awk -F 'node' '{print $NF}'))

CGROUP_NAME="htmm"
###### update DIR!
DIR=/home/taehyung/workspace/memtis/memtis-userspace

CONFIG_PERF=off
CONFIG_NS=off
CONFIG_NW=off
CONFIG_CXL_MODE=off
STATIC_DRAM=""
DATE=""
VER=""

function func_cache_flush() {
    echo 3 > /proc/sys/vm/drop_caches
    free
    return
}

function func_memtis_setting() {
    echo 199 | tee /sys/kernel/mm/htmm/htmm_sample_period
    echo 100007 | tee /sys/kernel/mm/htmm/htmm_inst_sample_period
    echo 1 | tee /sys/kernel/mm/htmm/htmm_thres_hot
    echo 2 | tee /sys/kernel/mm/htmm/htmm_split_period
    echo 100000 | tee /sys/kernel/mm/htmm/htmm_adaptation_period
    echo 2000000 | tee /sys/kernel/mm/htmm/htmm_cooling_period
    echo 2 | tee /sys/kernel/mm/htmm/htmm_mode
    echo 500 | tee /sys/kernel/mm/htmm/htmm_demotion_period_in_ms
    echo 500 | tee /sys/kernel/mm/htmm/htmm_promotion_period_in_ms
    echo 4 | tee /sys/kernel/mm/htmm/htmm_gamma
    ###  cpu cap (per mille) for ksampled
    echo 30 | tee /sys/kernel/mm/htmm/ksampled_soft_cpu_quota

    if [[ "x${CONFIG_NS}" == "xoff" ]]; then
	echo 1 | tee /sys/kernel/mm/htmm/htmm_thres_split
    else
	echo 0 | tee /sys/kernel/mm/htmm/htmm_thres_split
    fi

    if [[ "x${CONFIG_NW}" == "xoff" ]]; then
	echo 0 | tee /sys/kernel/mm/htmm/htmm_nowarm
    else
	echo 1 | tee /sys/kernel/mm/htmm/htmm_nowarm
    fi

    if [[ "x${CONFIG_CXL_MODE}" == "xon" ]]; then
	${DIR}/scripts/set_uncore_freq.sh on
	echo "enabled" | tee /sys/kernel/mm/htmm/htmm_cxl_mode
    else
	${DIR}/scripts/set_uncore_freq.sh off
	echo "disabled" | tee /sys/kernel/mm/htmm/htmm_cxl_mode
    fi

    echo "always" | tee /sys/kernel/mm/transparent_hugepage/enabled
    echo "always" | tee /sys/kernel/mm/transparent_hugepage/defrag
}

function func_prepare() {
    echo "Preparing benchmark start..."

	sudo sysctl kernel.perf_event_max_sample_rate=100000

	# disable automatic numa balancing
	sudo echo 0 > /proc/sys/kernel/numa_balancing
	# set configs
	func_memtis_setting
	
	DATE=$(date +%Y%m%d%H%M)

	export BENCH_NAME
	export NVM_RATIO

	if [[ "x${NVM_RATIO}" == "xstatic" ]]; then
	    if [[ "x${STATIC_DRAM}" != "x" ]]; then
		export STATIC_DRAM
	    fi
	fi

	if [[ -e ${DIR}/bench_cmds/${BENCH_NAME}.sh ]]; then
	    source ${DIR}/bench_cmds/${BENCH_NAME}.sh
	else
	    echo "ERROR: ${BENCH_NAME}.sh does not exist."
	    exit -1
	fi
}

function func_main() {
    ${DIR}/bin/kill_ksampled
    TIME="/usr/bin/time"

    if [[ "x${CONFIG_PERF}" == "xon" ]]; then
	PERF="./perf stat -e dtlb_store_misses.walk_pending,dtlb_load_misses.walk_pending,dTLB-store-misses,cycle_activity.stalls_total"
    else
	PERF=""
    fi
    
    # use 20 threads 
    PINNING="taskset -c 0-19"

    echo "-----------------------"
    echo "NVM RATIO: ${NVM_RATIO}"
    echo "${DATE}"
    echo "-----------------------"

    # make directory for results
    mkdir -p ${DIR}/results/${BENCH_NAME}/${VER}/${NVM_RATIO}
    LOG_DIR=${DIR}/results/${BENCH_NAME}/${VER}/${NVM_RATIO}

    # set memcg for htmm
    sudo ${DIR}/scripts/set_htmm_memcg.sh htmm remove
    sudo ${DIR}/scripts/set_htmm_memcg.sh htmm $$ enable
    sudo ${DIR}/scripts/set_mem_size.sh htmm 0 ${BENCH_DRAM}
    sleep 2

    # check dram size
    MAX_DRAM_SIZE=$(numastat -m | awk '$1 == "MemFree" { print int($2) }')
    if [[ ${BENCH_DRAM::-2} -gt ${MAX_DRAM_SIZE} ]]; then
	echo "Available DRAM size for ${BENCH_NAME} is only ${MAX_DRAM_SIZE}MB"
	echo "ERROR: abort -- change the ratio"
	exit -1
    fi

    cat /proc/vmstat | grep -e thp -e htmm -e pgmig > ${LOG_DIR}/before_vmstat.log 
    # flush cache
    func_cache_flush
    sleep 2

    ${DIR}/scripts/memory_stat.sh ${LOG_DIR} &
    if [[ "x${BENCH_NAME}" =~ "xsilo" ]]; then
	${TIME} -f "execution time %e (s)" \
	    ${PINNING} ${DIR}/bin/launch_bench_nopid ${BENCH_RUN} 2>&1 \
	    | tee ${LOG_DIR}/output.log
    elif [[ "x${BENCH_NAME}" =~ "xspeccpu" ]]; then
	${TIME} -f "execution time %e (s)" \
	    ${PINNING} ${DIR}/bin/launch_bench_nopid ${BENCH_RUN} < ${BENCH_ARG} 2>&1 \
	    | tee ${LOG_DIR}/output.log
    else
	${TIME} -f "execution time %e (s)" \
	    ${PINNING} ${DIR}/bin/launch_bench ${BENCH_RUN} 2>&1 \
	    | tee ${LOG_DIR}/output.log
    fi

    sudo killall -9 memory_stat.sh
    cat /proc/vmstat | grep -e thp -e htmm -e pgmig > ${LOG_DIR}/after_vmstat.log
    sleep 2

    if [[ "x${BENCH_NAME}" == "xbtree" ]]; then
	cat ${LOG_DIR}/output.log | grep Throughput \
	    | awk ' NR%20==0 { print sum ; sum = 0 ; next} { sum+=$3 }' \
	    > ${LOG_DIR}/throughput.out
    elif [[ "x${BENCH_NAME}" =~ "xsilo" ]]; then
	cat ${LOG_DIR}/output.log | grep -e '0 throughput' -e '5 throughput' \
	    | awk ' { print $4 }' > ${LOG_DIR}/throughput.out
    fi

    sudo dmesg -c > ${LOG_DIR}/dmesg.txt
    # disable htmm
    sudo ${DIR}/scripts/set_htmm_memcg.sh htmm $$ disable
}

function func_usage() {
    echo
    echo -e "Usage: $0 [-b benchmark name] [-s socket_mode] [-w GB] ..."
    echo
    echo "  -B,   --benchmark   [arg]    benchmark name to run. e.g., graph500, Liblinear, etc"
    echo "  -R,   --ratio       [arg]    fast tier size vs. capacity tier size: \"1:16\", \"1:8\", or \"1:2\""
    echo "  -D,   --dram        [arg]    static dram size [MB or GB]; only available when -R is set to \"static\""
    echo "  -V,   --version     [arg]    a version name for results"
    echo "  -NS,  --nosplit              disable skewness-aware page size determination"
    echo "  -NW,  --nowarm               disable the warm set"
    echo "        --cxl                  enable cxl mode [default: disabled]"
    echo "  -?,   --help"
    echo "        --usage"
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
	-V|--version)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		VER=( "$2" )
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-P|--perf)
	    CONFIG_PERF=on
	    shift 1
	    ;;
	-R|--ratio)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		NVM_RATIO="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-D|--dram)
	    if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
		STATIC_DRAM="$2"
		shift 2
	    else
		func_usage
		exit -1
	    fi
	    ;;
	-NS|--nosplit)
	    CONFIG_NS=on
	    shift 1
	    ;;
	-NW|--nowarm)
	    CONFIG_NW=on
	    shift 1
	    ;;
	--cxl)
	    CONFIG_CXL_MODE=on
	    shift 1
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
