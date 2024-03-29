
当前的 osd 把对象数据和日志数据放到一个磁盘里，使用一个 bdev 设备。当前osd支持两种bdev设备：aio和nvme。在配置文件里需要指定bdev的类型和磁盘。
osd有四个主要的参数：
 -C 指定osd配置文件
 --mkfs  表示要初始化磁盘
 --uuid  osd的uuid
 --id   osd id

其中“--mkfs”和“--uuid”这两个参数只有在初始化磁盘时才会用到。
启动osd之前必须先初始化磁盘，用于在磁盘里初始化一个blobstore。

“-C”指定的配置文件包含三部分，配置monitor、配置 OSD 自身和配置 RPC。关于 RPC 的配置说明，可以参考 `src/msg/README.md`。
配置文件的样例在源码的fastblock.json：

- **osds**  
    OSD 节点配置数组，里面可以包含多个 OSD,每个osd都是“osd_id”和磁盘的键值对 
- **monitors**  
    monitor 名字
- **mon_host**
    monitor地址


### 使用 Nvme 盘

#### 1 Spdk 接管磁盘 

```
sh setup.sh status   //查看磁盘的BDF
sh setup.sh 
```     

`setup.sh` 在 `fastblock/build/deps_build/spdk-prefix/src/spdk/scripts` 中。

#### 2 配置 json 文件

上一步 `setup.sh status` 获取了磁盘的 BDF，把此 BDF 值作为 json 文件的 traddr 值

`setup.sh status` 的结果，例：

```
Type     BDF             Vendor Device NUMA    Driver           Device     Block devices
NVMe     0000:03:00.0    15ad   07f0   0       nvme             nvme2      nvme2n1
NVMe     0000:0b:00.0    15ad   07f0   0       nvme             nvme1      nvme1n1
NVMe     0000:13:00.0    15ad   07f0   0       nvme             nvme0      nvme0n1
```  


磁盘 `/dev/nvme0n1` 的 BDF 为 `0000:13:00.0`

json文件的实例（部分）：

```json
    "osds": {
        "1": "0000:13:00.0"
    },
    "bdev_type": "nvme",
```

#### 3 启动 OSD
在启动一个新osd之前先要初始化磁盘

初始化磁盘命令：
```bash
fastblock/build/src/osd/fastblock-osd -s 1024 -m 0x1  -C fastblock.json  --mkfs --id 1 --uuid 1
```

启动osd命令：
```bash
fastblock/build/src/osd/fastblock-osd -s 1024 -m 0x1  -C fastblock.json  --id 1 
```


### 使用 Aio

#### 1 配置 json 文件

json 文件的实例(部分)：

```json
    "osds": {
        "1": "/tmp/aiofile"
    },
    "bdev_type": "aio",
```
 
注意：**json 文件里的  `/tmp/aiofile`必须大于 1GB，可以使用 `dd` 出来一个 2GB 的文件**

```bash
dd if=/dev/zero of=/tmp/aiofile bs=1G count=2
```
#### 2 启动 OSD
在启动一个新osd之前先要初始化磁盘

初始化磁盘命令：
```bash
fastblock/build/src/osd/fastblock-osd -s 1024 -m 0x1  -C fastblock.json  --mkfs --id 1 --uuid 1
```

启动命令：
```bash
fastblock/build/src/osd/fastblock-osd -s 1024 -m 0x1 -C fastblock.json --id 1
```

注：**OSD 初始化磁盘命令的"--uuid"是此 OSD 对应的 uuid，使用下面的命令向 monitor 注册（fbclient 是 monitor 编译出来的命令）**

```bash
fbclient -op=fakeapplyid -uuid=`uuidgen`
```
