threads=4
if [ $# == 1 ] ; then
    threads=$1
fi

export PATH=$PATH:/home/tassadar/arm-eabi-4.4.0/bin
make ARCH=arm CROSS_COMPILE=arm-eabi- -j$threads