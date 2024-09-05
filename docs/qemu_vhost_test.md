qemu对接fastblock-vhost以及虚拟机内部的数据一致性测试

# 0 注意
本文采用手动部署的方式部署集群及vhost服务, 如需快速部署，集群部署可以采用vstart.sh部署，vhost则可以使用fastblock-vhost.service快速启动.  
配置和启动fastblock-vhost.service见3.2.

# 1 环境
cpu:  Kunpeng-920   128核
系统： openEuler 22.03 (LTS-SP2)

# 2 部署fastblock集群
## 2.1 部署monitor
monitor运行在node1上，monitor配置文件 fastblock.json如下:
```json
{
  "monitors": ["monitor"],
  "mon_host": ["ip"],
  "log_path": "/var/log/fastblock/monitor1.log",
  "log_level": "info",
  "election_master_key": "fastblock_monitor_election"
}
```

启动monitor进程:  
```
fastblock-mon -conf=fastblock.json -id=monitor &
```
注意：这里的id要和配置文件中的monitors字段值一致

## 2.2 部署osd
osd的配置文件是json格式的：
```json
{
    "monitors": ["monitor"],
    "mon_host": ["ip"],
    "log_path": "/var/log/fastblock/monitor1.log",
    "log_level": "info",
    "election_master_key": "fastblock_monitor_election",
    "mon_rpc_address": "ip",
    "mon_rpc_port": 3333,
    "rdma_device_name": "rocep125s0f0",
    "msg_server_listen_backlog" : 1024,
    "msg_server_poll_cq_batch_size": 32,
    "msg_server_metadata_memory_pool_capacity": 8192,
    "msg_server_metadata_memory_pool_element_size": 1024,
    "msg_server_data_memory_pool_capacity": 8192,
    "msg_server_data_memory_pool_element_size": 8192,
    "msg_server_per_post_recv_num": 512,
    "msg_server_rpc_timeout_us": 1000000,
    "msg_client_poll_cq_batch_size": 32,
    "msg_client_metadata_memory_pool_capacity": 16384,
    "msg_client_metadata_memory_pool_element_size": 1024,
    "msg_client_data_memory_pool_capacity": 16384,
    "msg_client_data_memory_pool_element_size": 8192,
    "msg_client_per_post_recv_num": 512,
    "msg_client_rpc_timeout_us": 1000000,
    "msg_client_rpc_batch_size": 1024,
    "msg_client_connect_max_retry": 30,
    "msg_client_connect_retry_interval_us": 1000000,
    "msg_rdma_resolve_timeout_us": 2000,
    "msg_rdma_poll_cm_event_timeout_us": 1000000,
    "msg_rdma_max_send_wr": 4096,
    "msg_rdma_max_send_sge": 128,
    "msg_rdma_max_recv_wr": 8192,
    "msg_rdma_max_recv_sge": 1,
    "msg_rdma_max_inline_data": 16,
    "msg_rdma_cq_num_entries": 1024,
    "msg_rdma_qp_sig_all": false
}
```

配置3个osd
```
for osdid in 1 2 3
do
    uuid=`uuidgen`
    fastblock-client -op=fakeapplyid -uuid=${uuid}
    fastblock-osd -s 2048 -S 2 -C fastblock.json --mkfs --id ${osdid} --uuid ${uuid} --force 
    nohup fastblock-osd -s 2048 -C fastblock.json  --id ${osdid}  >> /var/log/fastblock/osd${osdid}.log  2>&1 &
done
```

## 2.3 创建pool和image
当3个osd都是up/in状态后，再创建pool
```
root@node1 /home/liuhongquan $ fastblock-client -op=getosdmap
OSDID        ADDRESS                PORT          STATUS-UP    STATUS-IN
1            175.5.24.252           9254          up           in
2            175.5.24.252           9641          up           in
3            175.5.24.252           9908          up           in
```

创建pool：
```
fastblock-client -op=createpool -poolname=fb -pgcount=6 -pgsize=3 -failure_domain="osd"
fastblock-client -op=createimage -imagesize=$((100*1024*1024*1024))  -imagename=fbimage -poolname=fb
```
# 3 启动vhost
## 3.1 使用命令行启动vhost
vhost的配置文件vhost.json：
```json
{
    "mon_host": ["175.5.24.252"],
    "mon_rpc_address": "175.5.24.252",
    "mon_rpc_port": 3333,
    "rdma_device_name": "rocep125s0f0",
    "msg_client_poll_cq_batch_size": 1024,
    "msg_client_metadata_memory_pool_capacity": 4096,
    "msg_client_metadata_memory_pool_element_size": 1024,
    "msg_client_data_memory_pool_capacity": 4096,
    "msg_client_data_memory_pool_element_size": 8192,
    "msg_client_per_post_recv_num": 512,
    "msg_client_rpc_timeout_us": 1000000,
    "msg_client_rpc_batch_size": 1024,
    "msg_client_connect_max_retry": 30,
    "msg_client_connect_retry_interval_us": 1000000,
    "msg_rdma_resolve_timeout_us": 2000,
    "msg_rdma_poll_cm_event_timeout_us": 1000000,
    "msg_rdma_max_send_wr": 1024,
    "msg_rdma_max_send_sge": 128,
    "msg_rdma_max_recv_wr": 8192,
    "msg_rdma_max_recv_sge": 1,
    "msg_rdma_max_inline_data": 16,
    "msg_rdma_cq_num_entries": 1024,
    "msg_rdma_qp_sig_all": false
}
```
启动vhost:
```
nohup fastblock/build/src/bdev/fastblock-vhost -m '[8]' -L libblk -C vhost.json >> /var/log/fastblock/bdev.log  2>&1 &
```
## 3.2 使用systemctl启动fastblock-vhost.service
配置文件/etc/fastblock/vhost.json如下:
```json
{
    "mon_host": ["175.5.24.252"],
    "mon_rpc_address": "175.5.24.252",
    "mon_rpc_port": 3333,
    "rdma_device_name": "rocep125s0f0",
    "msg_client_poll_cq_batch_size": 1024,
    "msg_client_metadata_memory_pool_capacity": 4096,
    "msg_client_metadata_memory_pool_element_size": 1024,
    "msg_client_data_memory_pool_capacity": 4096,
    "msg_client_data_memory_pool_element_size": 8192,
    "msg_client_per_post_recv_num": 512,
    "msg_client_rpc_timeout_us": 1000000,
    "msg_client_rpc_batch_size": 1024,
    "msg_client_connect_max_retry": 30,
    "msg_client_connect_retry_interval_us": 1000000,
    "msg_rdma_resolve_timeout_us": 2000,
    "msg_rdma_poll_cm_event_timeout_us": 1000000,
    "msg_rdma_max_send_wr": 1024,
    "msg_rdma_max_send_sge": 128,
    "msg_rdma_max_recv_wr": 8192,
    "msg_rdma_max_recv_sge": 1,
    "msg_rdma_max_inline_data": 16,
    "msg_rdma_cq_num_entries": 1024,
    "msg_rdma_qp_sig_all": false
```
启动fastblock-vhost.service:
```
systemctl start fastblock-vhost.service 
```

## 3.3 使用rpc.py创建bdev设备及vhost controler
创建bdev设备
```
fastblock/build/deps_build/spdk-prefix/src/spdk/scripts/rpc.py -s /var/tmp/bdev_vhost_192944.sock  bdev_fastblock_create -P 1 -p fb -i fbimage -k 4096 -I 100G -m "175.5.24.252:3333" -b fbdev
```
其中：-s /var/tmp/bdev_vhost_192944.sock 是/var/tmp目录下bdev_vhost_${vhost进程id}.sock

创建设备controller
```
fastblock/build/deps_build/spdk-prefix/src/spdk/scripts/rpc.py -s /var/tmp/bdev_vhost_192944.sock vhost_create_blk_controller /var/tmp/vhost.1 fbdev
```
创建完成后，会在/var/tmp下生成一个“vhost.1”文件，在启动qemu时会用到

# 4 启动qemu
## 4.1 安装qemu相关软件

```
yum install virt-manager qemu qemu-* libvirt -y
```

针对不同的架构，linux系统引导的方式有所差异:x86支持UEFI（Unified Extensible Firmware Interface）和BIOS方式启动，AArch64仅支持UEFI方式启动。统一的可扩展固件接口UEFI是一种全新类型的接口标准，用于开机自检、引导操作系统的启动。EDK II是一套实现了UEFI标准的开源代码，在虚拟化场景中，通常利用EDK II工具集，通过UEFI的方式启动虚拟机。
在鲲鹏920 + openeuler系统上，安装edk2:
```
yum install edk2-aarch64 edk2-devel.aarch64 -y
```
UEFI固件位置：/usr/share/edk2/aarch64/

## 4.2 启动qemu

从https://cloud.centos.org/centos/8-stream/aarch64/images/ 下载aarch64架构的CentOS云镜像：CentOS-Stream-GenericCloud-8-20240520.0.aarch64.qcow2

启动libvirtd服务：
```
systemctl start libvirtd
```

修改CentOS云镜像密码：
```
virt-customize -a CentOS-Stream-GenericCloud-8-20240520.0.aarch64.qcow2 --root-password password:123  
```

启动qemu:
```
taskset -c 36,37,38,39 qemu-system-aarch64 \
        -M virt \
        -cpu cortex-a57   \
        -smp 4 \
        -m 8G  \
        -object memory-backend-file,id=mem0,size=8G,mem-path=/dev/hugepages,share=on -numa node,memdev=mem0 \
        -drive file=/home/liuhongquan/CentOS-Stream-GenericCloud-8-20240520.0.aarch64.qcow2,id=hd0,if=none  \
        -device virtio-scsi-pci,id=scsi0  \
        -device scsi-hd,drive=hd0,bus=scsi0.0 \
        -bios /usr/share/edk2/aarch64/QEMU_EFI.fd \
        -device usb-ehci,id=usb \
        -device usb-tablet,bus=usb.0 \
        -chardev socket,id=spdk_vhost_blk0,path=/var/tmp/vhost.1 \
        -device vhost-user-blk-pci,chardev=spdk_vhost_blk0,num-queues=2 \
        -device virtio-net-pci,netdev=mynet0 \
        -netdev user,id=mynet0,hostfwd=tcp::10022-:22 \
        -vnc 0.0.0.0:1  \
        -nographic
```

其中：
    -cpu 为开启的虚拟机的cpu类型（qemu-system-aarch64 -cpu help查看支持的类型）
    -drive file 指定虚拟机镜像
    -bios  指定UEFI固件
    -chardev socket,id=spdk_vhost_blk0,path=/home/liuhongquan/vhost.1  中vhost.1就是上面创建设备controller生成的

# 5 虚拟机内进行数据一致性测试
## 5.1 mkfs并挂载
```
mkfs.xfs /dev/vda
mount /dev/vda /mnt
```

## 5.2 在/mnt目录下生成文件
使用python脚本file_genertator.py在/mnt目录下生成1个目录，里面有100个子目录，每个子目录下100个不同大小的文件:
```
import os
import random
import string
import hashlib
from concurrent.futures import ThreadPoolExecutor
import argparse

def generate_file(file_num, folder_path, file_size):
    # Generate random content for the file
    content = os.urandom(file_size)

    # Calculate the file's md5 hash
    file_md5 = hashlib.md5(content).hexdigest()

    # Generate the file name as a decimal number with 16 digits
    file_name = f"{file_num:016d}"

    # Write the content to the file
    file_path = os.path.join(folder_path, file_name)
    with open(file_path, "wb") as f:
        f.write(content)

    # Return the file path and its md5 hash
    return file_path, file_md5, file_size

def generate_files_in_folder(folder_num, num_files, file_sizes, file_size_probs, num_threads, folder_name):
    # Create the folder if it doesn't exist
    folder_path = os.path.join(folder_name, f"folder_{folder_num}")
    os.makedirs(folder_path, exist_ok=True)

    # Generate the files in parallel using threads
    with ThreadPoolExecutor(max_workers=num_threads) as executor:
        file_args = []
        for i in range(num_files):
            # Calculate the file number as a combination of the folder number and the file index
            file_num = folder_num * num_files + i
            # Choose the file size randomly based on the probabilities
            file_size = random.choices(file_sizes, weights=file_size_probs)[0]
            file_args.append((file_num, folder_path, file_size))
        # Generate the files in parallel and collect their results
        results = list(executor.map(lambda args: generate_file(*args), file_args))

    # Check the md5 of each file and print any files that have incorrect md5s
    incorrect_md5s = []
    for file_path, file_md5, file_size in results:
        with open(file_path, "rb") as f:
            if hashlib.md5(f.read()).hexdigest() != file_md5:
                correct_md5 = hashlib.md5(os.urandom(file_size)).hexdigest()
                incorrect_md5s.append((file_path, correct_md5, file_md5))
    if incorrect_md5s:
        print(f"Files with incorrect md5s in folder {folder_num}:")
        for file_path, correct_md5, incorrect_md5 in incorrect_md5s:
            print(f"File: {file_path}, Correct md5: {correct_md5}, Incorrect md5: {incorrect_md5}")

def generate_random_files(num_files, num_folders):
    # Generate a random folder name
    folder_name = ''.join(random.choices(string.ascii_lowercase, k=10))
    print(f"working dir is {folder_name}")

    # Create the base folder
    os.makedirs(folder_name, exist_ok=True)

    # Define the file sizes and their corresponding probabilities
    file_sizes = [2**i for i in range(12, 22)] # From 4KiB to 4MiB
    file_size_probs = [2**(-i+22) for i in range(12, 22)] # Smaller sizes have larger proportion

    # Define the number of threads to use for parallel file generation
    num_threads = num_folders

    # Generate the files in each folder in parallel
    for folder_num in range(num_folders):
        generate_files_in_folder(folder_num, num_files, file_sizes, file_size_probs, num_threads, folder_name)

if __name__ == "__main__":
    # Define the command line arguments
    generate_random_files(100,100)
```

通过以下目录生成:
```
$ python3 file_generator.py
working dir is rmwbhsjsll
```

## 5.3 拷贝目录并通过md5进行数据一致性验证
python脚本copy_and_compare.py如下
```
import os
import hashlib
import shutil
import argparse
import multiprocessing
import random
import string

def compute_md5(file_path):
    with open(file_path, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()

def compare_files(src_path, dest_path):
    src_md5 = compute_md5(src_path)
    if os.path.exists(dest_path):
        dest_md5 = compute_md5(dest_path)
    else:
        dest_md5 = None
    return (src_path, dest_path, src_md5, dest_md5)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Compare MD5 hashes of all files in a source folder with their corresponding files in a randomly named destination folder.')
    parser.add_argument('src_folder', metavar='src_folder', type=str, help='path to the source folder')
    parser.add_argument('-p', '--processes', type=int, default=4, help='number of processes to use')

    args = parser.parse_args()

    src_folder = args.src_folder
    dest_folder = os.path.join(os.getcwd(), ''.join(random.choices(string.ascii_letters, k=10)))

    print(f'Destination folder: {dest_folder}')

    # Copy the source folder to the destination folder
    shutil.copytree(src_folder, dest_folder)

    print(f'directory copied')

    pool = multiprocessing.Pool(args.processes)

    results = []
    for root, dirs, files in os.walk(src_folder):
        for file in files:
            src_path = os.path.join(root, file)
            dest_path = os.path.join(dest_folder, os.path.relpath(src_path, src_folder))
            results.append(pool.apply_async(compare_files, args=(src_path, dest_path)))

    pool.close()
    pool.join()

    mismatch = False

    for result in results:
        src_path, dest_path, src_md5, dest_md5 = result.get()
        if src_md5 != dest_md5:
            print(f'{os.path.abspath(src_path)} ({src_md5}) does not match {os.path.abspath(dest_path)} ({dest_md5})')
            mismatch = True

    # If there were no mismatches, print a message indicating that all files matched
    if not mismatch:
        print('All files matched!')

    # If there was a mismatch, print a warning message
    else:
        print('WARNING: There were files with non-matching MD5 hashes.')
```
通过以下命令行拷贝目录并进行前后的md5验证:
```
$ python3 copy_and_compare.py rmwbhsjsll
Destination folder: /mnt/hVLLIjzcOT
directory copied
All files matched!
```