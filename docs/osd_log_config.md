osd和vhost日志配置
# 背景
因为fastblock-osd和fastblock-vhost都是一个spdk app，这两个进程的日志都是将不同等级的日志写到rsyslog，然后由用户通过过滤、匹配rsyslog的内容，将日志写到对应的文件中. 
# rsyslog的配置
可以在/etc/rsyslog/conf.d中添加一个名为88-fastblock.conf的文件, 内容为:
```
if $msg contains "daemon_id : " then {
    :msg, contains, "daemon_id : 1" -/var/log/fastblock/osd1.log
    :msg, contains, "daemon_id : 2" -/var/log/fastblock/osd2.log
    :msg, contains, "daemon_id : 3" -/var/log/fastblock/osd3.log
    stop
}
```
其中匹配daemon_id是在osd和vhost代码中嵌入的标记id的关键词，不同的osd在初始化时就有不同的daemon_id, vhost则直接是-1. 

# 配置osd和vhost进程的日志等级
因为这两个进程都是spdk app，意味着可以通过spdk/scripts/rpc.py来配置日志等级，其中:
```
spdk/scripts/rpc.py -s /var/tmp/fastblock-osd-3.sock log_set_level "INFO"
spdk/scripts/rpc.py -s /var/tmp/fastblock-osd-3.sock log_set_flags all
```
前者是将日志等级设置为INFO，后者则是打开了所有日志模块的开关。  
因为日志等级会影响性能，所以在测试开发的时候可以适当提高日志等级，而需要进行性能测试的时候则降低日志等级。  