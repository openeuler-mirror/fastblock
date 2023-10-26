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

you should have a config file in `/etc/fastblock/monitor.toml` and make sure log dir is created, config file looks like:  
```
etcd_server = ["127.0.0.1:2379"] # Your etcd servers.
election_master_key = "fastblock_master"
hostname="monitor1"
port=1111
prometheus_port=1112
log_path = "/var/log/fastblock/monitor1.log"
log_level = "info"
```
you don't need to start a extra etcd cluster, we start a embeded etcd server.   

### cluster

We assume that the cluster has three nodes:

```
monitor1 192.168.1.1

monitor2 192.168.1.2

monitor3 192.168.1.3
```

#### monitor 1

```
etcd_server = ["192.168.1.1:2379", "192.168.1.2:2379", "192.168.1.3:3379"] # Your etcd servers.
etcd_name = "monitor1"
etcd_initial_cluster = "monitor1=http://192.168.1.1:2380,monitor2=http://192.168.1.2:2380,monitor3=http://192.168.1.3:2380"
etcd_advertise_client_urls = ["http://192.168.1.1:2379"]
etcd_advertise_peer_urls = ["http://192.168.1.1:2380"]
etcd_listen_peer_urls = ["http://192.168.1.1:2380"]
etcd_listen_client_urls = ["http://192.168.1.1:2379", "http://127.0.0.1:2379"]
election_master_key = "fastblock_master"
data_dir = "/tmp/monitor1"
hostname="monitor1"
port=3333
prometheus_port=1112
log_path = "/var/log/fastblock/monitor1.log"
log_level = "info"
```

#### monitor 2

```
etcd_server = ["192.168.1.1:2379", "192.168.1.2:2379", "192.168.1.3:3379"] # Your etcd servers.
etcd_name = "monitor1"
etcd_initial_cluster = "monitor1=http://192.168.1.1:2380,monitor2=http://192.168.1.2:2380,monitor3=http://192.168.1.3:2380"
etcd_advertise_client_urls = ["http://192.168.1.2:2379"]
etcd_advertise_peer_urls = ["http://192.168.1.2:2380"]
etcd_listen_peer_urls = ["http://192.168.1.2:2380"]
etcd_listen_client_urls = ["http://192.168.1.2:2379", "http://127.0.0.1:2379"]
election_master_key = "fastblock_master"
data_dir = "/tmp/monitor1"
hostname="monitor1"
port=3333
prometheus_port=1112
log_path = "/var/log/fastblock/monitor1.log"
log_level = "info"
```
#### monitor 3

```
etcd_server = ["192.168.1.1:2379", "192.168.1.2:2379", "192.168.1.3:3379"] # Your etcd servers.
etcd_name = "monitor1"
etcd_initial_cluster = "monitor1=http://192.168.1.1:2380,monitor2=http://192.168.1.2:2380,monitor3=http://192.168.1.3:2380"
etcd_advertise_client_urls = ["http://192.168.1.3:2379"]
etcd_advertise_peer_urls = ["http://192.168.1.3:2380"]
etcd_listen_peer_urls = ["http://192.168.1.3:2380"]
etcd_listen_client_urls = ["http://192.168.1.3:2379", "http://127.0.0.1:2379"]
election_master_key = "fastblock_master"
data_dir = "/tmp/monitor1"
hostname="monitor1"
port=3333
prometheus_port=1112
log_path = "/var/log/fastblock/monitor1.log"
log_level = "info"
```
