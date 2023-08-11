#!/bin/bash
IPADDR=10.211.55.5
PORT=8000

# setup file and bdev
for((i=1;i<=3;i++))
do
    bdevname=aiofile$i
    filepath=/tmp/"$bdevname"
    dd if=/dev/zero of=$filepath bs=128M count=4
    sed "s#BDEVNAME#$bdevname#g" sample_aio_bdev.json > /tmp/bdev_$i.json
    sed -i "s#FILENAME#$filepath#g" /tmp/bdev_$i.json
done

# make sure you have compiled monitor and installed
# start monitor
rm /tmp/etcddir -rf
monitor &
sleep 5
# apply osd ids
fbclient -op=fakeapplyid -uuid=1
fbclient -op=fakeapplyid -uuid=2
fbclient -op=fakeapplyid -uuid=3

# maybe configure huge page
# echo 2048 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
# maybe need to have rdma nic
# rdma link add rxe1 type rxe netdev enp0s5

# start osds
for ((i=1;i<=3;i++))
do
    # make sure osd is installed to system path
    osd -I $i -U $i -o $IPADDR -t $((PORT+i)) -m 0x1 -D aiofile$i -c /tmp/bdev_$i.json &
    sleep 10
done

# create a pool with once single pg
fbclient -op=createpool -poolname=asdf -pgcount=1 pgsize=3

sleep 10

# start opbench
# (fixme) we don't know whether osd 1 is the leader, we just try
opbench -o $IPADDR -t $PORT



