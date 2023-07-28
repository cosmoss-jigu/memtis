#!/bin/bash

sudo modprobe intel-uncore-frequency

if [[ "x$1" == "xon" ]]; then
    echo 800000 > /sys/devices/system/cpu/intel_uncore_frequency/package_01_die_00/max_freq_khz
    echo 800000 > /sys/devices/system/cpu/intel_uncore_frequency/package_01_die_00/min_freq_khz
elif [[ "x$1" == "xoff" ]]; then
    # default values
    echo 2400000 > /sys/devices/system/cpu/intel_uncore_frequency/package_01_die_00/max_freq_khz
    echo 1200000 > /sys/devices/system/cpu/intel_uncore_frequency/package_01_die_00/min_freq_khz
else
    echo "usage: ./set_uncore_freq.sh [on/off]"
fi
