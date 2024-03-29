# fastblock monitor
## how to build
need golang(maybe goproxy), just  
```
apt install golang
export GOPROXY=https://proxy.golang.com.cn,direct
make
```
monitor is used to store cluster metadata(to etcd) and distribute them(osdmap,pgmap etcd) to client(osds and clients).  
fbclient can fetch osdmap and pgmap, also can create osds and fake boot them.  
fakeosd is a fake osd to communicate with monitor.  
## how it works
monitor module store osdmap and pgmap to etcd, we use embeded etcd server so we don't need to start extra etcd clusters.  
only one leader monitor can work, others are standby, they are eletcted leader by using etcd api.  
client should implement a timer to fetch osdmap and pgmap to have full knowledge of the cluster state.  

## how to use it
fastblock-client -op=fakestartcluster can start a cluster so we can create pools to distribute the pgs  
fastblock-client -op=watchosdmap and -op=watchpgmap can watch on events of pool creations and deletions, so osd and client can handle map changes.  

### standalone

you should have a config file in `/etc/fastblock/monitor.json` and make sure log dir is created, config file looks like:  
```
{
    "monitors": ["fb41","fb42","fb43"],
    "mon_host": ["173.20.4.3","173.20.4.2","173.20.4.1"],
    "log_path": "/var/log/fastblock/monitor1.log",
    "log_level": "info",
    "election_master_key": "fastblock_monitor_election"
}
```
you don't need to start a extra etcd cluster, we start a embeded etcd server.   

### cluster

We assume that the cluster has three nodes:

```
monitor1 192.168.1.1

monitor2 192.168.1.2

monitor3 192.168.1.3
```

三个monitor的配置文件是一样的，其中“monitors”字段是monitor节点名
```
{
    "monitors": ["monitor1","monitor2","monitor3"],
    "mon_host": ["192.168.1.1","192.168.1.2","192.168.1.3"],
    "log_path": "/var/log/fastblock/monitor.log",
    "log_level": "info",
    "election_master_key": "fastblock_monitor_election"
}
```

启动monitor时需要使用参数“-id”参数指定monitor节点名
monitor1
  fastblock-mon -conf=fastblock.json -id=monitor1
monitor2
  fastblock-mon -conf=fastblock.json -id=monitor2
monitor3
  fastblock-mon -conf=fastblock.json -id=monitor3











