#!/bin/bash

CGROUP_NAME=$1
CGROUP_DIR=/sys/fs/cgroup
BASH_PID=$2

if [ "x$2" == "xremove" ]; then
	sudo rmdir ${CGROUP_DIR}/${CGROUP_NAME}
        exit
fi

if [ ! -d "${CGROUP_DIR}/${CGROUP_NAME}" ]; then
	sudo mkdir -p ${CGROUP_DIR}/${CGROUP_NAME}
fi

echo "+memory" | sudo tee ${CGROUP_DIR}/cgroup.subtree_control
echo "+cpuset" | sudo tee ${CGROUP_DIR}/cgroup.subtree_control

echo ${BASH_PID} | sudo tee ${CGROUP_DIR}/${CGROUP_NAME}/cgroup.procs
if [ "x$3" == "xenable" ]; then
    echo "enabled" | sudo tee ${CGROUP_DIR}/${CGROUP_NAME}/memory.htmm_enabled
    exit
elif [ "x$3" == "xdisable" ]; then
    echo "disabled" | sudo tee ${CGROUP_DIR}/${CGROUP_NAME}/memory.htmm_enabled
    exit
fi

echo "'set_htmm_memcg.sh' Invalid parameters...."
echo "./set_htmm_memcg.sh [cgroup name] [bash pid] [\"enable\" or \"disable\"]"
