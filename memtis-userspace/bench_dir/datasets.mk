
### dataset for Liblinear

DATASET_DIR = datasets/

.PHONY: dataset
dataset: ${DATASET_DIR} ${DATASET_DIR}/kdd12

${DATASET_DIR}:
	mkdir -p $@

${DATASET_DIR}/kdd12: ${DATASET_DIR}/kdd12.xz
	xz -d $^

${DATASET_DIR}/kdd12.xz:
	wget -P ${DATASET_DIR} https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/datasets/binary/kdd12.xz
