if [ ! -f ping_pong.pb.h ] || [ ! -f ping_pong.pb.cc ]; then
    protoc --cpp_out=. ping_pong.proto
else
    protoc --cpp_out=/tmp ping_pong.proto
    if ! cmp -s ping_pong.pb.h /tmp/ping_pong.pb.h || ! cmp -s ping_pong.pb.cc /tmp/ping_pong.pb.cc; then
        mv /tmp/ping_pong.pb.h ping_pong.pb.h
        mv /tmp/ping_pong.pb.cc ping_pong.pb.cc
    fi
fi
