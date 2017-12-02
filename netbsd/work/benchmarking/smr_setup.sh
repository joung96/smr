#!/usr/pkg/bin/bash

if [ $# -lt 4 ]
then
    echo "Usage: ./setup.sh [smr|hdd] [ffs|lfs] [stl|raw] [reset|noreset] (workload)"
    exit 1
fi

sudo umount /mnt/test 2> /dev/null

if [ "$1" == "smr" ]
then
    rdev="/dev/wd0a"
    dev="/dev/dk0"

    if [ "$4" == "reset" ]
    then
	# add -z flag for reset
	flags="-z"
    else
	flags=""
    fi
fi

if [ "$2" == "lfs" ]
then
    label="4.4LFS"
else
    label="4.2BSD"
fi


if [ "$3" == "stl" ]
then
    sudo ./smr-stl/format --map-bands 7 --group-bands 248 --over-provisioning 1.05 $rdev2
    #sudo ./smr-stl/format --map-bands 7 --group-bands 5 --over-provisioning 1.05 $rdev2
    sudo ./stl_attach $dev

    dev="/dev/stl0d"
    rdev="/dev/rstl0d"
fi

if [ "$2" == "lfs" ]
then
    # sector 4k?
    sudo newfs_lfs -B256M -f4K -b4K -w32  -F $flags $rdev

    # -n, don't start the cleaner, since we want to use /usr/src3/libexec
    sudo pkill -9 lfs_cleanerd
    sudo mount_lfs -n $dev /mnt/test
    #sudo /usr/src3/libexec/lfs_cleanerd/obj/lfs_cleanerd /mnt/test
else
    #sudo /usr/src3/sbin/newfs/obj/newfs -O2 -f4K -b4K -S4096 -s15269888 $rdev
    sudo newfs $rdev
    #sudo /usr/src3/sbin/newfs/obj/newfs -O2 -f4K -b4K -S4096 -s509000 $rdev

    sudo mount_ffs $dev /mnt/test
fi

if [ $# -eq 5 ]
then
    #sudo ./fb -f filebench/"$5".f
    #sudo fio --thread --ioengine=posixaio
    sudo fio --runtime=300 fio_workloads/"$5".fio
fi
