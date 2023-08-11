  当前的osd把对象数据和日志数据放到一个磁盘里，使用一个bdev设备。osd进程根据此json文件创建bdev。
  src/osd/conf/disk_bdev.json为json文件样例。

osd启动步骤：
1. 使用nvme盘
  - spdk接管磁盘 
    ```
    sh setup.sh status   //查看磁盘的BDF
    sh setup.sh 
	```     

    setup.sh在fastblock/build/deps_build/spdk-prefix/src/spdk/scripts中。

  - 配置json文件
   上一步 setup.sh status获取了磁盘的BDF，把此BDF值作为json文件的traddr值

     setup.sh status的结果，例  

          Type     BDF             Vendor Device NUMA    Driver           Device     Block devices
          NVMe     0000:03:00.0    15ad   07f0   0       nvme             nvme2      nvme2n1
          NVMe     0000:0b:00.0    15ad   07f0   0       nvme             nvme1      nvme1n1
          NVMe     0000:13:00.0    15ad   07f0   0       nvme             nvme0      nvme0n1  


      磁盘/dev/nvme0n1的BDF为0000:13:00.0
      json文件的实例：osd1_disk_bdev.json
    ```
    {
        "subsystems": [
          {
            "subsystem": "bdev",
            "config": [
              {
                "method": "bdev_nvme_attach_controller",
                "params": {
                  "name": "nvme0",
                  "trtype": "pcie",
                  "traddr": "0000:13:00.0"
                }
              }
            ]
          }
        ]
    }
    ```
  - 启动osd
    启动命令：  
    ```
      fastblock/build/src/osd/osd -I 1 -s 1024 -m 0x1 -o 192.168.23.129 -t 8888 -U 8978d071-030b-4929-b382-8a86608d7582 -D nvme0n1 -c osd1_disk_bdev.json
    ```
    参数-D为磁盘的名字，要和参数-c指定的json文件中的“traddr”对应

2. 使用aio
  - 配置json文件
    json文件的实例：osd1_disk_bdev.json
	```
	{
	  "subsystems": [
	    {
	      "subsystem": "bdev",
	      "config": [
	        {
	          "params": {
	            "name": "AIO0",
	            "block_size": 2048,
	            "filename": "/tmp/aiofile"
	          },
	          "method": "bdev_aio_create"
	        }
	      ]
	    }
	  ]
	}
	```
  注意：json文件里的filename指向的文件（既/tmp/aiofile）必须大于1GB，可以使用dd出来一个2GB的文件
  ```
  dd if=/dev/zero of=/tmp/aiofile bs=1G count=2
  ```
   - 启动osd
   启动命令
      ```
      fastblock/build/src/osd/osd -I 1 -s 1024 -m 0x1 -o 192.168.23.129 -t 8888 -U 8978d071-030b-4929-b382-8a86608d7582 -D AIO0 -c    osd1_disk_bdev.json
      ```

注：osd启动命令中的-U参数是此osd对应的uuid，使用下面的命令向monitor注册（fbclient是monitor编译出来的命令）
```
fbclient -op=fakeapplyid -uuid=`uuidgen`
```
