#!/bin/bash
pip3 install dfss

scripts_dir=$(dirname $(readlink -f "$0"))

pushd $scripts_dir

mkdir -p ../data

python3 -m dfss --url=open@sophgo.com:/sophon-stream/yolox_bytetrack_osd_encode/videos.tar
tar xvf videos.tar -C ../data
rm -rf videos.tar

mkdir -p ../data/models
python3 -m dfss --url=open@sophgo.com:/sophon-stream/yolov5/BM1684X.zip
unzip BM1684X.zip
rm -rf BM1684X.zip
mv ./BM1684X ../data/models/BM1684X

python3 -m dfss --url=open@sophgo.com:/sophon-stream/yolov5/BM1684X_tpukernel.zip
unzip BM1684X_tpukernel.zip
rm -rf BM1684X_tpukernel.zip
mv ./BM1684X ../data/models/BM1684X_tpukernel

python3 -m dfss --url=open@sophgo.com:/sophon-stream/yolox_bytetrack_osd_encode/BM1688.tar.gz
tar -zxvf BM1688.tar.gz
rm -rf BM1688.tar.gz
mv ./BM1688 ../data/models/BM1688

python3 -m dfss --url=open@sophgo.com:/sophon-stream/common/coco.names
mv ./coco.names ../data

popd
