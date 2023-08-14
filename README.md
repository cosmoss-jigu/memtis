# MEMTIS: Efficient Memory Tiering with Dynamic Page Classification and Page Size Determination

## System configuration
* Fedora 33 server
* Two 20-core Intel(R) Xeon(R) Gold 5218R CPU @ 2.10GHz
* 6 x 16GB DRAM per socket
* 6 x 128GB Intel Optane DC Persistent Memory per socket

MEMTIS currently supports two system configurations
* DRAM + Intel DCPMM (used only single socket)
* local DRAM + remote DRAM (used two socket, CXL emulation mode)

## Source code information
See linux/

You have to enable CONFIG\_HTMM when compiling the linux source.
```
make menuconfig
...
CONFIG_HTMM=y
...
```

### Dependencies
There are nothing special libraries for MEMTIS itself.

(You just need to install libraries for Linux compilation.)

## For experiments
### Userspace scripts
See memtis-userspace/

Please read memtis-userspace/README.md for detailed explanations

### Setting tiered memory systems with Intel DCPMM
* Reconfigures a namespace with devdax mode
```
sudo ndctl create-namespace -f -e namespace0.0 --mode=devdax
...
```
* Reconfigures a dax device with system-ram mode (KMEM DAX)
```
sudo daxctl reconfigure-device dax0.0 --mode=system-ram
...
```

### Preparing benchmarks
We used open-sourced benchmarks except SPECCPU2017.

We provided links to each benchmark source in memtis-userspace/bench\_dir/README.md

### Running benchmarks
It is necessary to create/update a simple script for each benchmark.
If you want to execute *XSBench*, for instance, you have to create memtis-userspace/bench\_cmds/XSBench.sh.

This is a sample.
```
# memtis-userspace/bench_cmds/XSBench.sh

BIN=/path/to/benchmark
BENCH_RUN="${BIN}/XSBench [Options]"

# Provide the DRAM size for each memory configuration setting.
# You must first check the resident set size of a benchmark.
if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="3850MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="7200MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="21800MB"
fi

# required
export BENCH_RUN
export BENCH_DRAM

```

#### Test
```
cd memtis-userspace/

# check running options
./scripts/run_bench.sh --help

# create an executable binary file
make

# run
sudo ./scripts/run_bench.sh -B ${BENCH} -R ${MEM_CONFIG} -V ${TEST_NAME}
## or use scripts
sudo ./run-fig5-6-10.sh
sudo ./run-fig7.sh
...
```

#### Tips for setting other tiered memory systems
See memtis-userspace/README.md

## Commit number used for artifact evaluation
174ca88

## License
<a rel="license" href="http://creativecommons.org/licenses/by-nc/4.0/"><img alt="Creative Commons License" style="border-width:0" src="https://i.creativecommons.org/l/by-nc/4.0/88x31.png" /></a><br />This work is licensed under a <a rel="license" href="http://creativecommons.org/licenses/by-nc/4.0/">Creative Commons Attribution-NonCommercial 4.0 International License</a>.

## Bibtex
To be updated 

## Authors
- Taehyung Lee (Sungkyunkwan University, SKKU) <taehyunggg@skku.edu>, <taehyung.tlee@gmail.com>
- Sumit Kumar Monga (Virginia Tech) <sumitkm@vt.edu>
- Changwoo Min (Virginia Tech) <changwoo@vt.edu>
- Young Ik Eom (Sungkyunkwan University, SKKU) <yieom@skku.edu>
