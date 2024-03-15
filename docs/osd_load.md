osd的启动命令是fastblock-osd，它有一个参数“-f [true, false]”，表示是新建，还是重启一个osd：
  true  表示新建一个osd，此时对应的磁盘必须是空的（把清空磁盘的任务交给用户，防止程序误清空磁盘）
  false 表示重启一个osd

osd是作为一个spdk app运行的，需要绑定cpu核，同时需要指定一个磁盘或文件（在实际项目中还是以磁盘为主，文件多用于测试，下面的介绍都是以磁盘为准）来创建bdev设备（具体怎么指定，在src/osd/readme.md中已经介绍）。

# 1 启动osd
osd的启动流程：
 - **创建bdev设备**
 bdev设备是在启动spdk app过程中根据参数“-c”指定的配置文件创建的，配置文件中指定了使用的磁盘。
 - **创建spdk内存池**
 buffer_pool_init函数用于创建一个spdk内存池（内部实际调用的是spdk_mempool_create），内存池大小为512MB，都是4K的内存块。
 spdk提供的读写blob的接口中存储数据的内存都必须是spdk创建的内存，为了提供内存使用效率，因此创建了个spdk内存池。
 - **初始化bdev设备**
 osd的本地存储都是基于spdk blobstrore进行存储。
 从bdev设备创建一个spdk blobstore块设备并初始化，这个过程是在blobstore_init函数中实现。
 osd的本地存储主要有raft log存储、对象存储和kv存储，都是通过spdk的blob进行存储，blob的扩展属性“type”里存储blob类型。

   raft log存储：存储raft log，每个pg(对应一个raft组)有一个spdk blob，blob类型为blob_type::log。
   对象存储：存储对象数据，每个对象有一个spdk blob，blob类型为blob_type::object。
   kv存储：保存当前cpu核上的需要保存的所有kv数据，包括raft的元数据、存储系统本身的数据。每个cpu核拥有一个spdk blob，blob类型为blob_type::kv。
在storage_init函数里创建kv类型的blob。
- **创建osd实例**
partition_manager是osd实体类，里面包含了pg group（管理pg）实例、osd_stm（操作对象）实例和osd状态，创建osd实例由pm_init完成，其实就是创建类partition_manager对象。
partition_manager::create_partition用于创建pg
partition_manager::delete_partition用于删除pg
partition_manager::change_pg_membership用于变更pg成员
partition_manager::load_partition用于加载pg
- **连接monitor**
连接monitor并向monitor发送BootRequest消息告知自己上线。
定期从monitor获取osd map和pg map。

```c++ protobuf
//BootRequest请求
message BootRequest {
    int32 osd_id = 1;
    string uuid = 2;
    int64 size = 3;
    uint32 port = 4;
    string address = 5;
    string host = 6;
}
```
获取osd map后会和里面的osd建立网络连接
获取pg map后会检查本地是否有此pg,是否是此pg的osd成员，根据需要决定创建pg、删除pg或更改pg成员。

- **启动osd实例**
启动osd实例由partition_manager::start完成：开启合并发送raft心跳的任务，然后设置osd状态为osd_state::OSD_ACTIVE
- **启动osd_service和raft_service网络服务器**
由函数service_init完成
osd_service网络服务器用于处理来自客户端的请求
raft_service网络服务器用于处理raft节点之间的raft请求
这两个网络服务都使用rdma

# 2 重启osd
osd的重启流程：
- **创建bdev设备**
bdev设备是在启动spdk app过程中根据参数“-c”指定的配置文件创建的，配置文件中指定了使用的磁盘。
- **创建spdk内存池**
  buffer_pool_init函数用于创建一个spdk内存池（内部实际调用的是spdk_mempool_create），内存池大小为512MB，都是4K的内存块。
spdk提供的读写blob的接口中存储数据的内存都必须是spdk创建的内存，为了提供内存使用效率，因此创建了个spdk内存池。
- **加载bdev设备**
osd的本地存储都是基于spdk blobstrore进行存储。
从bdev设备创建一个spdk blobstore块设备并加载blobstore，获取blobstore里的所有blob，根据blob扩展属性“type”按blob类型组织这些blob（放到blob_tree里），这个过程是在blobstore_load函数中实现。
- **加载KV**
从blob_tree找到kv对应的blob，加载blob kv，从此blob中读取kv数据到内存中。这个过程是在storage_load函数中完成。
- **创建osd实例**
partition_manager是osd实体类，里面包含了pg group（管理pg）实例、osd_stm（操作对象）实例和osd状态，创建osd实例由pm_init完
成，其实就是创建类partition_manager对象。
- **启动osd实例**
启动osd实例由partition_manager::start完成：开启合并发送raft心跳的任务，然后设置osd状态为osd_state::OSD_ACTIVE
- **加载pg**
blob_type::log和blob_type::object类型的blob的扩展属性“pg”会记录此blob属于哪个pg，从blob_tree里提取出各个pg的所有raft log和对象对应的blob去加载pg。从blob_type::object类型的blob中还原对象信息，从blob_type::log类型的blob中加载raft log信息。partition_manager::load_partition用于加载pg。
- **启动osd_service和raft_service网络服务器**
由函数service_init完成
osd_service网络服务器用于处理来自客户端的请求
raft_service网络服务器用于处理raft节点之间的raft请求
这两个网络服务都使用rdma
- **连接monitor**
连接monitor并向monitor发送BootRequest消息告知自己上线。
定期从monitor获取osd map和pg map。

kv blob和log blob中记录的数据是不断增长的，这两个blob空间是有限的，因此需要定期的回收空间。会有两个变量back和front分别指向有效数据的起始和结束位置，这两个变量存储在blob的super block中，但只要回收空间的时候才会去刷新这两个变量到super block，导致加载blob里的数据时，super block里的front可能并不是有效数据的结束位置，需要去读blob中front之后的数据去确定真实的front。