#!/bin/bash

CGROUP_NAME=$1
CGROUP_DIR=/sys/fs/cgroup
NODE_ID=$2
NODE_SIZE=$3
MAX=18446744073709551615

if [ $# -ne 3 ]; then
    echo "Not enough parameters..."
    echo "./set_mem_size.sh [cgroup name] [node id] [node size in (MB or GB) or \"max\"]"
    exit
fi

if [ ! -d "${CGROUP_DIR}/${CGROUP_NAME}" ]; then
    echo "error: there is no cgroup instance named \"${CGROUP_NAME}\""
    exit
fi

if [ ! -e "${CGROUP_DIR}/${CGROUP_NAME}/memory.max_at_node${NODE_ID}" ]; then
    echo "invalid node id: ${NODE_ID}"
    exit
fi

if [ "x${NODE_SIZE}" == "xmax" ]; then
    MEM_IN_BYTES=$MAX
else
    NODE_SIZE=${NODE_SIZE^^}

    if [[ ${NODE_SIZE} =~ "MB" ]]; then
	MEM_IN_BYTES=`echo ${NODE_SIZE::-2}*1024*1024 | bc`
    elif [[ ${NODE_SIZE} =~ "GB" ]]; then
	MEM_IN_BYTES=`echo ${NODE_SIZE::-2}*1024*1024*1024 | bc`
    fi
fi

echo ${MEM_IN_BYTES} | sudo tee ${CGROUP_DIR}/${CGROUP_NAME}/memory.max_at_node${NODE_ID}
