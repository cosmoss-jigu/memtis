
## compile 

.PHONY: benches
benches: gapbs/pr XSBench/openmp-threading/XSBench liblinear-multicore-2.45-1/train silo/out-perf.masstree/benchmarks/dbtest

PR_DIR = gapbs
XSB_DIR = XSBench/openmp-threading
LL_DIR = liblinear-multicore-2.45-1
SILO_DIR = silo

DIRS = ${PR_DIR} ${XSB_DIR} ${LL_DIR} ${SILO_DIR}

gapbs/pr: gapbs
	@pushd $^; \
	patch -p1 < ../gapbs-pr.diff; \
	popd; \
	${MAKE} -C $^; ${MAKE} -C $^ gen-twitter;

XSBench/openmp-threading/XSBench:
	${MAKE} -C ${XSB_DIR};
	
liblinear-multicore-2.45-1/train:
	@pushd ${LL_DIR}; \
	patch -p1 < ../liblinear.diff; \
	popd; \
	${MAKE} -C ${LL_DIR}; ${MAKE} -C ${LL_DIR} dataset;

silo/out-perf.masstree/benchmarks/dbtest:
	${MAKE} -C ${SILO_DIR} -j dbtest;

clean:
	@for d in $(DIRS); do \
		${MAKE} -C $$d clean; \
	done
