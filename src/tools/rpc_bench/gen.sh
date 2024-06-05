if [ ! -f rpc_bench.pb.h ] || [ ! -f rpc_bench.pb.cc ]; then
    protoc --cpp_out=. rpc_bench.proto
else
    protoc --cpp_out=/tmp rpc_bench.proto
    if ! cmp -s rpc_bench.pb.h /tmp/rpc_bench.pb.h || ! cmp -s rpc_bench.pb.cc /tmp/rpc_bench.pb.cc; then
        mv /tmp/rpc_bench.pb.h rpc_bench.pb.h
        mv /tmp/rpc_bench.pb.cc rpc_bench.pb.cc
    fi
fi
