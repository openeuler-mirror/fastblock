osd日志配置
# 背景
因为fastblock-osd是一个spdk app，这两个进程的日志都是将不同等级的日志写到rsyslog，当前版本中，我们使用systemctl重定向日志文件。
# systemctl service的配置
```
StandardOutput=append:/var/log/fastblock/osd%i.log
StandardError=append:/var/log/fastblock/osd%i.log
```
通过在fastblock-osd@.service中添加以下配置，可重定向日志文件。

# 配置osd和vhost进程的日志等级
因为这两个进程都是spdk app，意味着可以通过spdk/scripts/rpc.py来配置日志等级，其中:
```
spdk/scripts/rpc.py -s /var/tmp/fastblock-osd-3.sock log_set_level "INFO"
spdk/scripts/rpc.py -s /var/tmp/fastblock-osd-3.sock log_set_flags all
```
前者是将日志等级设置为INFO，后者则是打开了所有日志模块的开关。  
因为日志等级会影响性能，所以在测试开发的时候可以适当提高日志等级，而需要进行性能测试的时候则降低日志等级。  