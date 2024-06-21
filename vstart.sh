#!/bin/bash
set -x
# this script can deploy a fastblock cluster
# IMPORTANT NOTICE:
# 1. fastblock-osd/block_bench are spdk apps, performance will badly degraded if you run multiple daemons in the same core
# 2. if you want to use nvme bdevtype, your nvme disks should be held by spdk, disks are like 0000:d9:00.0(check docs/ dir for details)
# 3. you should run `make install` in build dir to install required binaries and systemctl service files
# 4. there are two modes of deployment, dev and pro:
#   4.1 dev mode is for development, will deploy a whole new cluster in localhost, mainly for testing/debugging
#   4.2 in dev mode, monitor ip address and osd disks are not required, will use local file as backend of osd
#   4.3 in dev mode, a default pool will be created after cluster is deployed
#   4.4 pro mode is for production, you may run this script multiple times to add multiple osds in different machines
#   4.5 in pro mode, you should provide monitor ip address or disks for osds
#   4.6 in pro mode, since using a virtual rdma nic doesn't make any sense, you should provide a rdma nic name
# 5. ssh passwordless login should be configured between machines to add remote osds
# 6. if rdma nic is not provided, we will use a virutal rdma nic(by kernel module rdma_rxe) 
# 7. if some osds failed to start because of lacking of hugepage, you should apply enough hugepage before running this script

osdcount=0
pgcount=8
replica=3
mode="dev"
nic=""
disks=""
bdevtype="aio"
remoteIp=""
defaultConfigFile="/etc/fastblock/fastblock.json"


installPrefix="/usr/local/bin"
osdBinary=$installPrefix"/fastblock-osd"
monBinary=$installPrefix"/fastblock-mon"
benchBinary=$installPrefix"/block_bench"

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -M, --Mon: create monitor with provided monitor ip address"
    echo "  -c, --osdcount: osd count, default=0"
    echo "  -p, --pgcount: pg count of the default pool in dev mode, default=8"
    echo "  -i, --ip: host ip address of the disk"
    echo "  -r, --replica: replica count, default=3"
    echo "  -n, --nic: rdma nic name"
    echo "  -d, --disks: disks separated by comma(,)"
    echo "  -t, --bdevtype: type of bdev, can be nvme or aio, default to aio"
    echo "  -m, --mode: deployment mode, can be dev or pro, default to dev"
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
        -p|--pgcount)
        shift
            pgcount="$1"
            ;;
        -r|--replica)
        shift
            replica="$1"
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
        -i|--ip)
        shift
            remoteIp="$1"
            ;;
        -m|--mode)
        shift
            mode=$1
            ;;
        -M|--Mon)
        shift
            monString=$1
            ;;
        *)
            echo "Unknown argument: $1"
            usage
            ;;
    esac
    shift
done

function isOfedInstaled() {
    if [ ! -f /usr/bin/ofed_info ]; then
        echo "false"
    fi
}

# 9.check system memory, if memory is enough, we don't need to modify rdma message parameters
function isSmallMemory() {
    total_memory=$(free -g | awk '/^Mem:/{print $2}')
    if [ "$total_memory" -lt 64 ]; then
        echo "true"
    fi
}

# 2.generate a new config file for monitor
function generateConfigAndStartMonitor() {
    _mons=$1
    mkdir -p /var/log/fastblock
    mkdir -p /etc/fastblock
    tmpConfFile=$(mktemp)

    # default msg_rdma_cq_num_entries is 16, make it larger if you encounter CQ errors
    echo '{"msg_rdma_cq_num_entries": 1024}' | jq '.' > $tmpConfFile

    rm -rf /var/lib/fastblock/mon_"$_mons"
    jq '.monitors |= ['\"$_mons\"']' $tmpConfFile > tmp_file.json && mv tmp_file.json $tmpConfFile
    jq '.mon_host |= ['\"$_mons\"']' $tmpConfFile > tmp_file.json && mv tmp_file.json $tmpConfFile
    jq '.mon_rpc_address |= '\"$_mons\"'' $tmpConfFile > tmp_file.json && mv tmp_file.json $tmpConfFile
    jq '.mon_rpc_port |= 3333' $tmpConfFile > tmp_file.json && mv tmp_file.json $tmpConfFile
    jq '.log_path |= "/var/log/fastblock/monitor.log"' $tmpConfFile > tmp_file.json && mv tmp_file.json $defaultConfigFile

    mv $tmpConfFile /etc/fastblock
    echo "moving config file to /etc/fastblock!"

    # stop any existing monitor service and start monitor service
    systemctl stop fastblock-mon.target
    systemctl start fastblock-mon@"$_mons".service
    echo "monitor started!"
    
    # wait until monitor's 3333 port is ready
    while ! nc -z $_mons 3333; do
        sleep 1
    done
    sleep 5
}


function createNewDevCluster() {
    if [ $monString = "" ] ;then
        monString="127.0.0.1"
    fi
    generateConfigAndStartMonitor $monString
    generateNicConfig

    for i in `seq 1 $osdcount`
    do
        # according to the definition of addNewOsd, the parameters are:
        #_disk_path=$1
        #_osdid=$2
        #_uuid=$3
        #_bdev_type=$4
        #_remote_ip=$5
        # in a dev cluster, all osds are local, disk_path is the osd work dir 
       
        uuid=`uuidgen`
        id=$(/usr/local/bin/fastblock-client -op=fakeapplyid -uuid=$uuid -endpoint="$monString":3333)
        addNewOsd "" $id $uuid $bdevtype ""
        sleep 30
    done
    echo "all osds started!"

    echo "create default pool fb"
    sleep 20
    /usr/local/bin/fastblock-client -op=createpool -poolname=fb -pgcount=$pgcount -pgsize=$replica -endpoint="$monString":3333
}


function processProCluster() {
    if [ "$monString" = "" ] && [ ! -f $defaultConfigFile ];then
        echo "create osd need a existing created /etc/fastblock/fastblock.json"
    fi

    if [ "$monString" != "" ];then
        generateConfigAndStartMonitor $monString
    else 
        ms=$(jq '.mon_host[0]' $defaultConfigFile)
        if [ "$?" != "0" ]; then
            echo "failed to parse monitor ip address from /etc/fastblock/fastblock.json"
            exit 1
        fi
        ms=$(echo $ms | sed 's/^"//; s/"$//')
        configExist=$(ssh $remoteIp '[ -f /etc/fastblock/fastblock.json ] && echo true')
        if [ "$configExist" != "true" ];then
            cp $defaultConfigFile /tmp/fastblock.json
            scp /tmp/fastblock.json $remoteIp:$defaultConfigFile
        fi

        echo "monitor ip address is $ms"

        # each host can have different nic name
        ssh $remoteIp "jq '.rdma_device_name = \"$nic\"' $defaultConfigFile > tmp_file.json && mv tmp_file.json $defaultConfigFile"
        IFS=',' read -r -a device_array <<< "$disks"

        for ((i=0; i<${#device_array[@]}; i++)); do
            _uuid=`uuidgen`
            id=$(/usr/local/bin/fastblock-client -op=fakeapplyid -uuid=$_uuid -endpoint="$ms":3333)
            addNewOsd "${device_array[$i]}" $id $_uuid $bdevtype $remoteIp
        done
    fi
}



function generateNicConfig() {
    if [ "$nic" = "" ];then
        nic=$(ip link show | grep UP | grep -v lo | awk -F': ' '{print $2}')
        rdma link add rdmanic type rxe netdev $nic
        jq '.rdma_device_name |= "rdmanic"' $defaultConfigFile > tmp_file.json && mv tmp_file.json $defaultConfigFile
    else
        jq '.rdma_device_name |= '\"$nic\"'' $defaultConfigFile > tmp_file.json && mv tmp_file.json $defaultConfigFile
    fi
    isSm=$(isSmallMemory)
    echo $isSm
    if [ "$isSm" = "true" ];then
        echo "small memory, need modify memory pool capacity"
        jq '.msg_server_metadata_memory_pool_capacity |= 16' $defaultConfigFile > tmp_file.json && mv tmp_file.json $defaultConfigFile
        jq '.msg_server_data_memory_pool_capacity |= 16' $defaultConfigFile > tmp_file.json && mv tmp_file.json $defaultConfigFile
        jq '.msg_client_metadata_memory_pool_capacity |= 16' $defaultConfigFile > tmp_file.json && mv tmp_file.json $defaultConfigFile
        jq '.msg_client_data_memory_pool_capacity |= 16' $defaultConfigFile > tmp_file.json && mv tmp_file.json $defaultConfigFile
    fi
}


function parametersCheck(){
    if [ ! -f $osdBinary ] || [ ! -f $monBinary ] || [ ! -f $benchBinary ]; then
        echo "fastblock binaries are not installed, please run make install in build dir"
        exit 1
    fi

    if [ "$mode" != "dev" ] && [ "$mode" != "pro" ];then
        echo "mode should be dev or pro"
        exit 1
    fi

    if [ "$nic" != "" ];then
        if [ "$remoteIp" == "" ];then
            nic_count=`rdma link | grep $nic | grep LINK_UP | wc -l`
            if [ "$?" != 0 ] || [ "$nic_count" != 1 ];then
                echo "no nic or nic is not up"
                exit 1
            fi
        else
            nic_count=$(ssh $remoteIp "rdma link | grep $nic | grep LINK_UP | wc -l")
            if [ "$?" != 0 ] || [ "$nic_count" != 1 ];then
                echo "remote no nic or nic is not up"
                exit 1
            fi

        fi
    fi

    # in dev mode, we only accept monString, osdcount, pgcount, replica
    # bdevtype, nic, disks, remote_ip are not allowed, because:
    # 1. bdevtype is always aio in dev mode because it's enough to run a dev osd;
    # 2. nic is not required in dev mode, we will use a virtual rdma nic;
    # 3. disks is not required in dev mode, we will use local file as backend of osd;
    # 4. remote_ip is not required in dev mode because all osds are local;
    if [ "$mode" = "dev" ]; then
        if [ "$disks" != "" ];then
            echo "in dev mode, disks should not be provided"
            exit -1
        fi

        if [ "$bdevtype" != "aio" ];then
            echo "in dev mode, bdevtype should be aio"
            exit -1
        fi

        if [ "$remoteIp" != "" ];then
            echo "in dev mode, remoteIp should not be provided"
            exit -1
        fi

        if [ "$osdcount" = 0 ];then
            echo "in dev mode, osdcount should not be 0"
            exit -1
        fi

        # when no nic provided, we will use a virtual rdma nic, so we need to check if rdma_rxe can be loaded
        # note that in a physical machine, rdma_rxe may not be loaded when you already have ofed installed and service started
        # in a physical machine with rdma nic, ofed must be installed and service started
        if [ "$nic" = "" ];then
            modprobe rdma_rxe
            if [ "$?" != "0" ]; then
                echo "modprobe rdma_rxe failed, specify a rdma nic or check if ofed is installed correctly"
                exit 1
            fi
        fi
    else
        if [ "$osdcount" != 0 ];then
            echo "in pro mode, osdcount should not be be provided"
            exit -1
        fi


        # either create monitor or osd in a single run, in this case, we do monitor creation only
        if [ "$monString" != "" ] && [ "$disks" != "" ]; then
            echo "can't create monitor and osd in the same run, please create monitor first, the run vstart.sh without monString specified"
            exit -1
        fi

        if [ "$disks" != "" ] && [ "$remoteIp" = "" ]; then
            echo "must specify IP address of the host when create osd(s)"
            exit -1
        fi


        if [ "$monString" != "" ];then
            echo "going to create monitor $monString"
        else
            # parse defaultConfigFile to get monitor ip address
            ms=$(jq '.mon_host[0]' $defaultConfigFile)
            if [ "$?" != "0" ]; then
                echo "monitor is not running on $ms , please start monitor first"
                exit 1
            fi
            ms=$(echo $ms | sed 's/^"//; s/"$//')

            # ssh to the monString and check whether monitor is running
            ssh $ms "systemctl status fastblock-mon@$ms.service"


            # check whether monitor's 3333 port is ready
            while ! nc -z $ms 3333; do
                sleep 1
            done

            sleep 5

            if [ "$disks" = "" ];then
                echo "in pro mode, disks must be provided to create osd" 
                exit 1
            fi

            if [ "$nic" = "" ];then
                echo "in pro mode, nic must be provided"
                exit 1
            fi

            if [ "$bdevtype" != "nvme" ] && [ "$bdevtype" != "aio" ];then
                echo "in bdev mode, bdevtype should be nvme or aio"
                exit 1
            fi
        fi
    fi
}

# disk(bdev) config path
# 6.prepare empty file for each osd, this file will be the backend of a spdk bdev device
# because raft log used 1GB space(for now, will optimized in the future), so the file should not be too small

function addNewOsd() {
    _disk_path=$1
    _osdid=$2
    _uuid=$3
    _bdev_type=$4
    _remote_ip=$5

    if [ "$_remote_ip" = "" ];then
        rm -rf /var/lib/fastblock/osd-"$_osdid"
        mkdir -p /var/lib/fastblock/osd-"$_osdid"
        bdev_type_path=/var/lib/fastblock/osd-"$_osdid"/bdev_type
        disk_path=/var/lib/fastblock/osd-"$_osdid"/disk
        echo $_bdev_type > $bdev_type_path

        if [ "$_disk_path" = "" ];then
            rm -f /var/lib/fastblock/osd-"$_osdid"/file
            truncate -s 100G /var/lib/fastblock/osd-"$_osdid"/file
            echo /var/lib/fastblock/osd-"$_osdid"/file > $disk_path
        else
            echo $_disk_path > $disk_path
        fi

        echo intializing localstore of osd-"$_osdid"
        /usr/local/bin/fastblock-osd -m '[1]' -C $defaultConfigFile --id $_osdid --mkfs --force --uuid $_uuid

        echo "starting osd-$_osdid"
        systemctl start fastblock-osd@"$_osdid".service
    else
        echo "starting remote osd-" $_osdid "on" $_remote_ip
        ssh $_remote_ip "rm -rf /var/lib/fastblock/osd-$_osdid"
        ssh $_remote_ip "mkdir -p /var/lib/fastblock/osd-$_osdid"
        ssh $_remote_ip "echo $_bdev_type > /var/lib/fastblock/osd-$_osdid/bdev_type"
        ssh $_remote_ip "echo $_disk_path > /var/lib/fastblock/osd-$_osdid/disk"

        if [ "$_disk_path" = "" ];then
            ssh $_remote_ip "rm -f /var/lib/fastblock/osd-$_osdid/file"
            ssh $_remote_ip "truncate -s 100G /var/lib/fastblock/osd-$_osdid/file"
            ssh $_remote_ip "echo /var/lib/fastblock/osd-$_osdid/file > /var/lib/fastblock/osd-$_osdid/disk"
        else
            ssh $_remote_ip "echo $_disk_path > /var/lib/fastblock/osd-$_osdid/disk"
        fi

        if [ "$_bdev_type" = "nvme" ] ;then
            ssh $_remote_ip "rm -f /var/tmp/spdk_pci_lock_$_disk_path"
        fi

        echo intializing localstore of osd-"$_osdid" "on" $_remote_ip
        ssh $_remote_ip "/usr/local/bin/fastblock-osd -m '[1]' -C $defaultConfigFile --id $_osdid --mkfs --force --uuid $_uuid"
        echo "starting osd-$_osdid on $_remote_ip"
        ssh $_remote_ip "systemctl start fastblock-osd@$_osdid.service"
    fi
}

# do sanity check first
parametersCheck

if [ "$mode" = "dev" ];then
    createNewDevCluster
else
    processProCluster
fi
