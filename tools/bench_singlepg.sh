#!/bin/bash
IPADDR=172.31.77.144
PORT=8000
TOTAL_OSDS=3

# setup file and bdev
for((i=1;i<=$TOTAL_OSDS;i++))
do
    bdevname=aiofile$i
    filepath=/tmp/"$bdevname"
    dd if=/dev/zero of=$filepath bs=512M count=8
    sed "s#BDEVNAME#$bdevname#g" sample_aio_bdev.json > /tmp/bdev_$i.json
    sed -i "s#FILENAME#$filepath#g" /tmp/bdev_$i.json
done

# make sure you have compiled monitor and installed
# start monitor
killall -9 /usr/local/bin/monitor
killall -9 /usr/local/bin/osd
rm /tmp/etcddir -rf
monitor &
sleep 5
# apply osd ids

for((i=1;i<=$TOTAL_OSDS;i++))
do
    fbclient -op=fakeapplyid -uuid=$i
done

# maybe configure huge page
# echo 2048 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
# maybe need to have rdma nic
# rdma link add rxe1 type rxe netdev enp0s5

# start osds
for ((i=1;i<=$TOTAL_OSDS;i++))
do
    # make sure osd is installed to system path
    # not in the same core
    # if you are using a nvme bdev, bdev name should + "n1"
    osd -I $i -U $i -o $IPADDR -t $((PORT+i)) -m 0x$((2**(i-1))) -D aiofile"$i" -c /tmp/bdev_$i.json -r /var/tmp/socket.$i.sock &
    sleep 10
done

# create a pool with once single pg
fbclient -op=createpool -poolname=asdf -pgcount=1 pgsize=$TOTAL_OSDS

# sleep 10

# start opbench
# (fixme) we don't know whether osd 1 is the leader, we just try
# fbbench -o 172.31.77.144 -t 8001 -S 4096 -E 11111 -m 0x8 -T rpc
# fbbench -o 172.31.77.144 -t 8001 -S 4096 -E 11111 -m 0x8 -T write



