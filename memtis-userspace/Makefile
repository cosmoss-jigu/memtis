CC = gcc
CFLAGS = -g -Wall -O3

DIR = bin
default: $(DIR) launch_bench launch_bench_nopid
all: $(DIR) launch_bench launch_bench_nopid benches

$(DIR):
	mkdir -p $@

launch_bench_nopid: launch_bench.c
	${CC} ${CFLAGS} -D __NOPID launch_bench.c -o ${DIR}/launch_bench_nopid

launch_bench: launch_bench.c
	${CC} ${CFLAGS} launch_bench.c -o ${DIR}/launch_bench

include bench_dir/Makefile

clean:
	rm launch_bench launch_bench_nopid
