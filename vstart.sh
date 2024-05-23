#!/bin/bash
set -x
# this script can start a fastblock cluster, and can start a randwrite benchmark
# NOTICE:
# fastblock-osd/block_bench are spdk apps, performance will badly degraded if you run multiple daemons in the same core
# if you want to use nvme bdevtype, your nvme disks should be held by spdk, disks are like 0000:d9:00.0(check docs/ dir for details)
# this script will always create a new cluster, original data will be wiped if any
# this script doesn't do much sanity checks

osdcount=3
replica=3
mt="vm"
nic=""
disks=""
bdevtype="aio"
runBenchmark=false
usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -c, --count: osd count, default=3"
    echo "  -r, --replica: replica count, default=3"
    echo "  -m, --machine: machine type, can be physical or vm, default to vm"
    echo "  -n, --nic: rdma nic name, must be provided in physical machine type"
    echo "  -d, --disks: disks separated by comma(,)"
    echo "  -t, --bdevtype: type of bdev, can be nvme or aio, default to aio"
    echo "  -b, --benchmark: whether run a benchmark after"
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
        -m|--machine)
	    shift
            mt="$1"
            ;;
        -n|--nic)
	    shift
            nic="$1"
            ;;
        -d|--disks)
	    shift
            disks="$1"
            ;;
        -t|--bdevtype)
	    shift
            bdevtype="$1"
            ;;
        -b|--benchmark)
	    shift
            runBenchmark=true
            ;;
        *)
            echo "Unknown argument: $1"
            usage
            ;;
    esac
    shift
done

# check parameters, we have following rules:
# 1.machine type can only be physical and vm;
# 2.physical machine should provide a rdma nic;
# 3.physical machine should have ofed driver installed;
# 4.vm usually don't have disks, so we will create tmp files as their bdev backend just to run a cluster;
# 5.physical machine may have disks, you can config there bdev type(aio/nvme);

if [ "$mt" != "physical" ] && [ "$mt" != "vm" ];then
    echo "machine type should be either pyhical or vm"
    exit 1
fi

if [ "$mt" = "physical" ] && [ "$nic" = "" ];then
    if [ "$nic" = "" ];then
        echo "in physical machine, you should provide you rdma nic"	
		exit 1
    fi
    if [ ! -f /usr/bin/ofed_info ]; then
        echo "in physical machine, you should have ofed driver instaled"	
		exit 1
    fi
    exit 1
fi

if [ "$mt" = "physical" ] && [ "$nic" = "" ];then
    if [ "$nic" = "" ];then
        echo "in physical machine, you should provide you rdma nic"	
		exit 1
    fi
    if [ ! -f /usr/bin/ofed_info ]; then
        echo "in physical machine, you should have ofed driver instaled"	
		exit 1
    fi
    exit 1
fi


ROOT=$(pwd)
killall -9 $ROOT/monitor/fastblock-mon
killall -9 $ROOT/build/src/osd/fastblock-osd

# 1.monitor data will be located in /tmp dir by default
rm /tmp/mon* -r

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
if [ "$disks" = "" ];then
    json="{"
    for i in `seq 1 $osdcount`
    do
	truncate -s 4G ./file"$i"
        if [ $i -eq 1 ]; then
            json="$json\"$i\":\"file$i\""
        else
            json="$json,\"$i\":\"file$i\""
        fi
    done
    json="$json}"
    jq ".osds = $json" vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
else
    # in this case, machine type must be physical
    IFS=',' read -r -a device_array <<< "$disks"
    if [ ${#device_array[@]} -ne $osdcount ]; then
        echo "disk number doesn't match osd count"
        exit 1
    fi
    json="{"
    for ((i=0; i<${#device_array[@]}; i++)); do
        id=$((i+1))
        json="$json\"$id\":\"${device_array[$i]}\""
        if [ $i -lt $((${#device_array[@]}-1)) ]; then
            json="$json,"
        fi
        if [ $bdevtype != "nvme" ];then
		    dd if=/dev/zero of="${device_array[$i]}" bs=4M count=1024
		fi
    done
    json="$json}"


    jq ".osds = $json" vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
    if [ $bdevtype = "nvme" ];then
         jq '.bdev_type = "nvme"' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
    fi
fi

# 7.apply enough huge page to run osd
echo 4096 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages

# 8.use a non-lo as rdma nic and config osds nic
# notice: if you started ofed service, modprobe rdma_rxe will fail
if [ "$mt" = vm ];then
    echo "i'm a vm"
    modprobe rdma_rxe
    nic=$(ip link show | grep UP | grep -v lo | awk -F': ' '{print $2}')
    rdma link add rdmanic type rxe netdev $nic
    jq '.rdma_device_name |= "rdmanic"' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json

    # a virtual machine should use less rmda memory, so we change *_capacity to 16
    jq '.msg_server_metadata_memory_pool_capacity |= 16' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
    jq '.msg_server_data_memory_pool_capacity |= 16' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
    jq '.msg_client_metadata_memory_pool_capacity |= 16' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
    jq '.msg_client_data_memory_pool_capacity |= 16' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
else
    echo "i'm a physical machine"
    jq '.rdma_device_name |= '\"$nic\"'' vm_fastblock.json > tmp_file.json && mv tmp_file.json vm_fastblock.json
fi

# 9.call mkfs to initiate osd's localstore
for i in `seq 1 $osdcount`
do
    # adding --force parameter means the disk's spdk blobstore  will be totally wiped out without mercy
    # if --force is not added, mkfs will failed if you have a fastblock superblob in the disk's superblock
    $ROOT/build/src/osd/fastblock-osd -m '['$i']' -C vm_fastblock.json --id $i --mkfs --force --uuid $i
done


# 10.start osd
for i in `seq 1 $osdcount`
do
	$ROOT/build/src/osd/fastblock-osd -m '['$i']' -C vm_fastblock.json --id $i &
    sleep 20
done
    

# you may check osd state from etcd
# osd topology is stored in etcd by monitor in plain text
#ETCDCTL_API=3 etcdctl get --prefix "" --endpoints=127.0.0.1:2379

if [ $runBenchmark = true ]; then
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
    
    if [ "$mt" = vm ];then
        jq '.msg_client_metadata_memory_pool_capacity |= 16' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
        jq '.msg_client_data_memory_pool_capacity |= 16' vm_blockbench.json > tmp_file.json && mv tmp_file.json vm_blockbench.json
    fi
    
    $ROOT/build/src/tools/block_bench -m '[4]'  -C vm_blockbench.json
fi
