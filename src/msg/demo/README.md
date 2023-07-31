# RDMA RPC DEMO

## 1 编译

```
$ make -j client server
```

## 2 运行

**服务端**

```
$ LD_LIBRARY_PATH=<spdk so lib path>:$LD_LIBRARY_PATH ASAN_OPTIONS=alloc_dealloc_mismatch=0 LD_PRELOAD=/usr/local/lib64/libasan.so.6.0.0 src/msg/demo/server -H <listen ip> -P <listen port>
```

**客户端**

```
$ LD_LIBRARY_PATH=<spdk so lib path>:$LD_LIBRARY_PATH ASAN_OPTIONS=alloc_dealloc_mismatch=0 LD_PRELOAD=/usr/local/lib64/libasan.so.6.0.0 src/msg/demo/client -H <server ip> -P <server port>
```
