# RPC

*RPC* 模块协议层使用 *protobuf* 实现，传输层使用 *rdma cm api* 实现，目前仅支持在 *RDMA* 网卡上运行。

一般来说，*RPC* 模块对外主要有两个头文件，*src/msg/rdma/client.h* 和 *src/msg/rdma/server.h*，分别包含了客户端和服务的的实现。

## 1 使用

示例可以参考 *src/msg/demo* 目录下的代码。

服务端和客户端的配置文件细节可以参考 *src/msg/README.md*。

### 1.1 服务端

```c++
auto opts = msg::rdma::server::make_options(
  g_pt.get_child("msg").get_child("server"),
  g_pt.get_child("msg").get_child("rdma"));
opts->bind_address =
  g_pt.get_child("bind_address").get_value<std::string>();
opts->port = g_pt.get_child("bind_port").get_value<uint16_t>();
```

静态函数 `make_options()` 从 *json* 中读取对应的配置。

```c++
ctx->server = std::make_shared<msg::rdma::server>(g_cpumask, opts);
ctx->server->add_service(ctx->rpc_service);
ctx->server->start();
```

创建 `msg::rdma::server` 实例后，注册 *rpc service*，并启动。

### 1.2 客户端

```c++
auto opts = msg::rdma::client::make_options(
  g_pt.get_child("msg").get_child("client"),
  g_pt.get_child("msg").get_child("rdma"));
auto g_rpc_client = std::make_shared<msg::rdma::client>("rpc_cli", &g_cpumask, opts);
g_rpc_client->start();
```

客户端启动流程和服务的类似。

```c++
g_rpc_client->emplace_connection(
  g_pt.get_child("server_address").get_value<std::string>(),
  g_pt.get_child("server_port").get_value<uint16_t>(),
  [] (bool is_ok, std::shared_ptr<msg::rdma::client::connection> conn) {
      if (not is_ok) {
          throw std::runtime_error{"create connection failed"};
      }
      
      // .......
      g_conn = conn;
      g_stub = std::make_unique<ping_pong::ping_pong_service_Stub>(g_conn.get());
      // ......
      g_stub->ping_pong(/* ...... */);
      // ......
  }
);
```

这一步是和服务端建立连接，第三个参数是回调函数，当连接建立或创建失败时会调用。在回调函数中创建 *rpc service* 实例，并调用 *rpc* 方法，即上示代码中的 `g_stub->ping_pong`。

## 2 实现

*RPC* 对外的接口都是异步的。内部实现采用 *spdk poller +* 队列的方式。服务端和客户端在启动时，均会预先 *post* 配置中指定数量的 *Receive WR*，并创建内存池。

客户端在调用 `g_stub->ping_pong()` 后，最终会进入到 `msg::rdma::client::connection::CallMethod()` 方法里。在这里，会对 *rpc* 请求序列化，序列化后的二进制数据会按配置中配置的内存块大小进行切块，同时，相关切片数量等元信息也会写入到另一个较小的内存块中，这部分实现 `transport_data.h` 文件中。然后将 *rpc* 请求栈生产到请求队列。

当用于消费 *rpc* 请求的 *spdk poller* 发现请求队列不为空时，取出队头，将本次 *rpc* 请求所需的内存块数量等一系列元数据通过 *RDMA Send* 发送到服务端。

服务端负责 *poll CQ* 的 *spdk poller* 在 *poll* 到服务端的 *Send WR* 后，根据元信息，准备好接收 *rpc* 的内存块，通过 *RDMA Read* 读取客户端的 *rpc* 请求。在服务端，另有一个 *spdk poller* 会不断校验用于读取 *rpc* 请求的内存块的首尾字节，当两个字节表示 *RDMA Read* 已经完成时，服务端将 *rpc* 请求拼接并进行反序列化，然后再执行该 *rpc* 请求。

执行完成后，*rpc* 的返回值也会以相同的方式进行序列化，而后服务端将元信息通过 *RDMA
 Send* 发送给客户端，客户端 *poll* 到服务端的 *Send WR* 后，和服务端的流程一样，对服务端发起 *RDMA Read* 操作。而后同样的，客户端的 *spdk poller* 在确认 *RDMA Read* 完成后，会向服务端发送一个仅包含 *imm data* 的 *Send WR*，用以告诉服务端，序列号位 *imm data* 的请求已经完成，服务端收到后会释放相关资源。
 
 之后客户端饭序列化 *response*，并返回给上层。
