# RDMA RPC DEMO

## 1 demo

```
$ make -j client server
```

**服务端**

```
$ src/msg/demo/server -C <json conf path>
```

**客户端**

```
$ src/msg/demo/client -C <json conf path>
```

## 2 ping_pong

该代码为 *rpc* 端的压测代码，配置文件为同目录下的 *ping_pong.json*，其中，需要说明的部分如下：

```json
"endpoints": [
    {
        "index": 1,
        "list": [
            {"host": "172.31.4.143", "port": 8081}
        ]
    },
    {
        "index": 2,
        "list": [
            {"host": "172.31.4.143", "port": 8082}
        ]
    },
    {
        "index": 3,
        "list": [
            {"host": "172.31.4.143", "port": 8083}
        ]
    }
]
```

这里有 *3* 个节点，需要启动 *3* 个 *ping_pong* 进程，每个进程需要 *3* 颗核，一颗核给 *rpc server*，另外两颗给 *rpc client*，去连除自己以外的其他进程。连接建立后，每个客户端都会发送 *io_count* 个 *rpc* 请求。

运行命令为：

```
$ src/msg/demo/ping_pong -C src/msg/demo/ping_pong.json -I 1 -m [10,11] -L ping_pong -L msg
$ src/msg/demo/ping_pong -C src/msg/demo/ping_pong.json -I 2 -m [12,13] -L ping_pong -L msg
$ src/msg/demo/ping_pong -C src/msg/demo/ping_pong.json -I 3 -m [14,15] -L ping_pong -L msg
```

*-I* 用于指定 *json* 中的 *index* 字段。

### 3 使用 supervisord 托管 ping_pong

在 *scripts* 目录下，有段 *python* 代码可以生成 *supervisord* 使用的配置，命令如下：

```
$ python3 scripts/pp_sup_gen.py <conf_path>/src/msg/demo/ping_pong.json <bin_path>/src/msg/demo/ping_pong
```

生成的配置文件为 `/etc/supervisor/conf.d/ping_pong.conf`，如下:

```bash
[group:ping_pong_debug]
programs=ping_pong_debug_1,ping_pong_debug_2


[program:ping_pong_debug_1]
command=<bin_path>/src/msg/demo/ping_pong -C <conf_path>/src/msg/demo/ping_pong.json -I 1 -m [10,11] -L ping_pong -L msg
autostart=false
autorestart=false
startsecs=5
startretries=3
redirect_stderr=true
stdout_logfile_maxbytes = 1GB
stdout_logfile=/var/log/ping_pong/ping_pong_debug_1.log
stderr_logfile=/var/log/ping_pong/ping_pong_debug_1_err.log
pre-start = /bin/bash -c ">/var/log/ping_pong/ping_pong_debug_1.log;>/var/log/ping_pong/ping_pong_debug_1_err.log"

[program:ping_pong_debug_2]
command=<bin_path>/src/msg/demo/ping_pong -C <conf_path>/src/msg/demo/ping_pong.json -I 2 -m [12,13] -L ping_pong -L msg
autostart=false
autorestart=false
startsecs=5
startretries=3
redirect_stderr=true
stdout_logfile_maxbytes = 1GB
stdout_logfile=/var/log/ping_pong/ping_pong_debug_2.log
stderr_logfile=/var/log/ping_pong/ping_pong_debug_2_err.log
pre-start = /bin/bash -c ">/var/log/ping_pong/ping_pong_debug_2.log;>/var/log/ping_pong/ping_pong_debug_2_err.log"

[program:ping_pong_debug_3]
command=<bin_path>/src/msg/demo/ping_pong -C <conf_path>/src/msg/demo/ping_pong.json -I 3 -m [16,17,18] -L ping_pong -L msg
autostart=false
autorestart=false
startsecs=5
startretries=3
redirect_stderr=true
stdout_logfile_maxbytes = 1GB
stdout_logfile=/var/log/ping_pong/ping_pong_debug_3.log
stderr_logfile=/var/log/ping_pong/ping_pong_debug_3_err.log
pre-start = /bin/bash -c ">/var/log/ping_pong/ping_pong_debug_3.log;>/var/log/ping_pong/ping_pong_debug_3_err.log"
```

然后使用 `supervisorctl` 启动：

```
$ supervisorctl start ping_pong_debug:*
```
