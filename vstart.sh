#!/bin/bash
# this script can start a fastblock cluster without rdma nic, and start a randwrite benchmark
# please notice:
# 1. if you have a rdma nic, you don't need to do procedure 8, you should start ofed service instead;
# 2. vm should have enough memory and cpu to run multiple osds;
# 3. this script create a new cluster each time when you run it, you can just stop/restart osds if you want to perserve your data
# 4. if you want to run benchmark after cluster is created, the *_capacity parameters should also change to 16 just as procedure 10 did

osdcount=3
replica=3
usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -c, --count: osd count, default=3"
    echo "  -r, --replica: replica count, default=3"
    exit 1
}

if [ "$#" -eq 0 ] || [ "$1" = "--help" ]; then
    usage
fi

# Loop through the command-line arguments
while [ "$#" -gt 0 ]; do
    case "$1" in
        -c|--count)
	    shift
            osdcount="$1"
            ;;
        -r|--replica)
	    shift
            replica="$1"
            ;;
        *)
            echo "Unknown argument: $1"
            usage
            ;;
    esac
    shift
done


ROOT=$(pwd)
killall -9 $ROOT/monitor/fastblock-mon
killall -9 $ROOT/build/src/osd/fastblock-osd

# 1.monitor data will be located in /tmp dir by default
rm /tmp/mon* -rf

# 2.generate a nice config file for monitor
hostname=$(hostname -s)
cp fastblock.json vm_fastblock.json
jq '.monitors |= ['\"$hostname\"']' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
jq '.mon_host |= ["127.0.0.1"]' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
jq '.log_path |= "./monitor.log"' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json

# 3.start a monitor
$ROOT/monitor/fastblock-mon -conf=./vm_fastblock.json -id=$hostname &

# 4.wait a slow peroid for monitor to start tcp monitor service
sleep 10

# 5.apply id for osd
for i in `seq 1 $osdcount`
do
	$ROOT/monitor/fastblock-client -op=fakeapplyid -uuid=$i -endpoint=127.0.0.1:3333
done

# 6.prepare empty file for each osd, this file will be the backend of a spdk bdev device
# because raft log used 1GB space(for now, will optimized in the future), so the file should not be too small
for i in `seq 1 $osdcount`
do
	dd if=/dev/zero of=./file"$i" bs=4M count=1024
done
jq '.osds |= {"1": "file1","2":"file2","3":"file3"}' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json

# 7.apply enough huge page to run osd
echo 4096 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages

# 8.use a non-lo as rdma nic and config osds nic
# notice: if you started ofed service, modprobe rdma_rxe will fail
modprobe rdma_rxe
nic=$(ip link show | grep UP | grep -v lo | awk -F': ' '{print $2}')
rdma link add rdmanic type rxe netdev $nic
jq '.rdma_device_name |= "rdmanic"' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json

# 9.call mkfs to initiate osd's localstore
for i in `seq 1 $osdcount`
do
	$ROOT/build/src/osd/fastblock-osd -m '['$i']' -C vm_fastblock.json --id $i --mkfs --uuid $i
done

# 10.a virtual machine should use less rmda memory, so we change *_capacity to  16
jq '.msg_server_metadata_memory_pool_capacity |= 16' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
jq '.msg_server_data_memory_pool_capacity |= 16' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
jq '.msg_client_metadata_memory_pool_capacity |= 16' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
jq '.msg_client_data_memory_pool_capacity |= 16' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json

# 10.start osd
for i in `seq 1 $osdcount`
do
	$ROOT/build/src/osd/fastblock-osd -m '['$i']' -C vm_fastblock.json --id $i &
	sleep 20
done

# you may check osd state from etcd
# osd topology is stored in etcd by monitor in plain text
#ETCDCTL_API=3 etcdctl get --prefix "" --endpoints=127.0.0.1:2379

# 11.create pool and image
size=$((1024*1024*1024))
$ROOT/monitor/fastblock-client -op=createpool -poolname=fb -pgcount=1 -pgsize=$replica -endpoint=127.0.0.1:3333
$ROOT/monitor/fastblock-client -op=createimage -imagesize=$size  -imagename=fbimage -poolname=fb -endpoint=127.0.0.1:3333

sleep 20
# now cluster is created and pool/image is ready, we can run block_bench with a edited blockbench.json
cp $ROOT/src/tools/block_bench.json vm_blockbench.json
jq '.mon_host |= ["127.0.0.1"]' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
jq '.image_name |= "fbimage"' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
jq '.pool_name |= "fb"' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
jq '.pool_id |= 1' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
jq '.object_size |= 4194304' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
jq '.image_size |= '\"$size\"'' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
jq '.msg_client_metadata_memory_pool_capacity |= 16' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
jq '.msg_client_data_memory_pool_capacity |= 16' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json

$ROOT/build/src/tools/block_bench -m '[4]'  -C vm_blockbench.json
