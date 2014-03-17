#!/bin/bash
rm output/kernel/zImage
export KBUILD_BUILD_VERSION="v7.6"
make -j6
make modules
make zImage
find . -name '*.ko' -exec cp {} output/system/lib/modules/  \;
cp arch/arm/boot/zImage /root/k2-3.4.10-gae62ace/output/kernel/zImage
cd /root/k2-3.4.10-gae62ace/output
zip -r DreamBeats_Kernel-v7.6.zip *
