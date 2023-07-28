# install benchmarks
## PageRank
```
git clone https://github.com/sbeamer/gapbs.git

# compile it with twitter dataset
cd gapbs
patch -p1 < ../gapbs-pr.diff
make pr; make pr gen-twitter
```

## Graph500
```
wget https://github.com/graph500/graph500
```

## XSBench
```
git clone https://github.com/ANL-CESAR/XSBench.git
```

## Liblinear
```
wget https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/multicore-liblinear/liblinear-multicore-2.45-1.zip

# apply patch before compiling liblinear
cd liblinear-multicor-2.45-1
patch -p1 < ../liblinear.diff

# Liblinear requires a dataset; you can get kdd12 dataset by compiling it with dataset option
make dataset

# or down it from web
wget https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/datasets/binary/kdd12.xz
```

## Btree
```
git clone https://github.com/mitosis-project/vmitosis-workloads.git

# change the number of elements and lookup requests
vim btree/btree.c
# see line 61
```
