# how monitor works
monitors are cheap, they are woring as active/standby mode,  
we can start many of them(may be 3 is enough), and only one of them are working,   
that means only the leader monitor can process tcp requests and react with etcd.  
The clients and osds know the list of all the monitors when startup,   
if the original monitor disconnected, they can simply reconnect with other  
monitors by reiterate the list.  
Monitor does not store data, all the data are store in etcd, so a new started  
monitor can simply fetch data from etcd and start work.
