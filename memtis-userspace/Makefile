CC = gcc
CFLAGS = -g -Wall -O3

DIR = bin
default: $(DIR) launch_bench launch_bench_nopid kill_ksampled

$(DIR):
	mkdir -p $@

launch_bench_nopid: launch_bench.c
	${CC} ${CFLAGS} -D __NOPID launch_bench.c -o ${DIR}/launch_bench_nopid

launch_bench: launch_bench.c
	${CC} ${CFLAGS} launch_bench.c -o ${DIR}/launch_bench

kill_ksampled: kill_ksampled.c
	${CC} ${CFLAGS} launch_bench.c -o ${DIR}/kill_ksampled

clean:
	rm ${DIR}/launch_bench ${DIR}/launch_bench_nopid ${DIR}/kill_ksampled
