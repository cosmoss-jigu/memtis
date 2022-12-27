# MEMTIS userspace

## Benchmarks
see bench\_dir/README.md

## Using Intel Optane DC PMM
```
sudo daxctl reconfigure-device dax0.0 --mode=system-ram
sudo daxctl reconfigure-device dax1.0 --mode=system-ram
```

## build
```
make
```
or
```
make all
# This additionally build the benchmarks
```

## run
see options
```
./scripts/run_bench.sh --help
```
execute
```
sudo ./run.sh
```

## Setting for other systems
### Limits the DRAM size
* AutoNUMA, AutoTiering, Tiering-0.8, TPP: use memmap option
    * e.g., grubby --args="memmap=000M\!000M" --update-kernel=${kernel path or number}
* Nimble: memory cgroup option
* HeMem: compile time setting
    * change $DRAMSIZE (hemem.h)
    * change the length threshold in mmap\_filter() (interpose.c) if necessary

### Setting
#### HeMem
* Link: <https://bitbucket.org/ajaustin/hemem/src/sosp-submission/>
* Before running benchmarks, you need to check the additional DRAM usage due to small allocations
* In some cases (e.g., btree), we decrease the length threshold for small allocations for two reasons
    * First, to control the DRAM size (especially when the available DRAM size is extremely low)
    * Second, we sure that those small alloations are not ephemeral
    * For example, we set the threshold to 128MB for the Btree benchmark (default: 1GB)
```
time LD_PRELOAD=/path/libhemem.so numactl -N 0 [benchmark]
```

#### Nimble
* Link (kernel): <https://github.com/ysarch-lab/nimble_page_management_asplos_2019>
* Link (userspace): <https://github.com/ysarch-lab/nimble_page_management_userspace>
* It is necessary to create (or update) bench directories and files (refer to end\_to\_end\_launcher/graph500-omp)
* Checkout kernel repo to 5.6 version
```
# run_all.sh
...
export FAST_NODE=0
export SLOW_NODE=2
...
BENCHMARK_LIST= NEED TO UPDATE
...
MEM_SIZE_LIST= NEED TO UPDATE
...
```
```
# launcher.c
...
int syscall_mm_manage = 440;
...
```

#### AutoTiering
* Link: <https://github.com/csl-ajou/AutoTiering>
* It is necessary to create (or update) bench files (see autotiering-userspace/benches/\_bench\_name.sh)
    * The number of threads, benchmark options, ...
* It also needs to update migration path in run-bench.sh
```
# run-bench.sh
...
for node in ${MEM_NODES[@]}; do
    case $node in
	0)
	    func_config_migration_path $node -1
	    func_config_promotion_path $node -1
	    func_config_demotion_path  $node 2
	    ;;
	1)
	    func_config_migration_path $node -1
	    func_config_promotion_path $node -1
	    func_config_demotion_path  $node 3
	    ;;
	2)
	    func_config_migration_path $node -1
	    func_config_promotion_path $node 0
	    func_config_demotion_path  $node -1
	    ;;
	3)
	    func_config_migration_path $node -1
	    func_config_promotion_path $node 1
	    func_config_demotion_path  $node -1
	    ;;
	*)
	    err "Add NUMA node$node interfaces"
	    ;;
    esac
done
...
```
```
# use --socket 0 when running benchmarks
./run-bench.sh --benchmark [arg] -wss [arg] --max-threads [arg; e.g., 16] --iter 3 --socket 0 --sequence a --opm bg --thp
```

#### TPP
* Link: <https://lore.kernel.org/all/cover.1637778851.git.hasanalmaruf@fb.com/>
```
sudo echo 1 > /sys/kernel/mm/numa/demotion_enabled
sudo echo 3 > /proc/sys/kernel/numa_balancing
```

#### Tiering-0.8
* Link: <https://git.kernel.org/pub/scm/linux/kernel/git/vishal/tiering.git/>
```
#### default setting

sudo echo 1 > /sys/kernel/mm/numa/demotion_enabled
## enable numa balancing for promotion
sudo echo 2 > /proc/sys/kernel/numa_balancing
sudo echo 30 > /proc/sys/kernel/numa_balancing_rate_limit_mbps

## enable early wakeup
sudo echo 1 > /proc/sys/kernel/numa_balancing_wake_up_kswapd_early

## enable decreasing hot threshold if the pages just demoted are hot
sudo echo 1 > /proc/sys/kernel/numa_balancing_scan_demoted
sudo echo 16 > /proc/sys/kernel/numa_balancing_demoted_threshold
```

#### AutoNUMA
* use vanila kernel
```
sudo echo 1 > /proc/sys/kernel/numa_balancing
```
