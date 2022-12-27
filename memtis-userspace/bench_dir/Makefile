
## compile 

.PHONY: benches
benches: gapbs/pr XSBench/openmp-threading/XSBench liblinear-multicore-2.45-1/train silo/out-perf.masstree/benchmarks/dbtest

DIRS = gapbs XSBench/openmp-threading liblinear-multicore-2.45-1 silo

gapbs/pr: gapbs
	@pushd $^; \
	patch -p1 < ../gapbs-pr.diff; \
	popd; \
	${MAKE} -C $^; ${MAKE} -C $^ gen-twitter;

XSBench/openmp-threading/XSBench:
	${MAKE} -C XSBench/openmp-threading;
	
liblinear-multicore-2.45-1/train:
	@pushd $^; \
	patch -p1 < ../liblinear.diff; \
	popd; \
	${MAKE} -C $^; ${MAKE} -C $^ dataset;

silo/out-perf.masstree/benchmarks/dbtest:
	${MAKE} -C $@ -j dbtest;

clean:
	@for d in $(DIRS); do \
		${MAKE} -C $$d clean; \
	done
