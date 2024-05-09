#!/bin/bash

echo "try to build proto files"

buildtype="cpp"
usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -t, --buildtype : can be golang or cpp"
    exit 1
}

if [ "$#" -eq 0 ] || [ "$1" = "--help" ]; then
    usage
fi

# Loop through the command-line arguments
while [ "$#" -gt 0 ]; do
    case "$1" in
        -t|--buildtype)
	    shift
            buildtype="$1"
            ;;
        *)
            echo "Unknown argument: $1"
            usage
            ;;
    esac
    shift
done

echo $buildtype
if [ $buildtype != "cpp" ] && [ $buildtype != "golang" ];then
	echo "buildtype only can be golang or cpp"
	exit 1
fi

if [ $buildtype = "cpp" ]; then
    if [ ! -f ../src/rpc/common_msg.pb.h ] || [ ! -f ../src/rpc/common_msg.pb.cc ]; then
        protoc --cpp_out=../src/rpc common_msg.proto
    else
        protoc --cpp_out=/tmp common_msg.proto
        if ! cmp -s ../src/rpc/common_msg.pb.h /tmp/common_msg.pb.h || ! cmp -s ../src/rpc/common_msg.pb.cc /tmp/common_msg.pb.cc; then
            mv /tmp/common_msg.pb.h ../src/rpc/common_msg.pb.h
            mv /tmp/common_msg.pb.cc ../src/rpc/common_msg.pb.cc
        fi
    fi

    if [ ! -f ../src/rpc/raft_msg.pb.h ] || [ ! -f ../src/rpc/raft_msg.pb.cc ]; then
        protoc --cpp_out=../src/rpc raft_msg.proto

    else
        protoc --cpp_out=/tmp raft_msg.proto
        if ! cmp -s ../src/rpc/raft_msg.pb.h /tmp/raft_msg.pb.h || ! cmp -s ../src/rpc/raft_msg.pb.cc /tmp/raft_msg.pb.cc; then
            mv /tmp/raft_msg.pb.h ../src/rpc/raft_msg.pb.h
            mv /tmp/raft_msg.pb.cc ../src/rpc/raft_msg.pb.cc
        fi
    fi


    if [ ! -f ../src/rpc/osd_msg.pb.h ] || [ ! -f ../src/rpc/osd_msg.pb.cc ]; then
        protoc --cpp_out=../src/rpc osd_msg.proto
    else
        protoc --cpp_out=/tmp osd_msg.proto
        if ! cmp -s ../src/rpc/osd_msg.pb.h /tmp/osd_msg.pb.h || ! cmp -s ../src/rpc/osd_msg.pb.cc /tmp/osd_msg.pb.cc; then
            mv /tmp/osd_msg.pb.h ../src/rpc/osd_msg.pb.h
            mv /tmp/osd_msg.pb.cc ../src/rpc/osd_msg.pb.cc
        fi
    fi

    if [ ! -f ../src/monclient/messages.pb.h ] || [ ! -f ../src/monclient/messages.pb.cc ]; then
        protoc --cpp_out=../src/monclient messages.proto
    else
        protoc --cpp_out=/tmp messages.proto
        if ! cmp -s ../src/monclient/messages.pb.h /tmp/messages.pb.h || ! cmp -s ../src/monclient/messages.pb.cc /tmp/messages.pb.cc; then
            mv /tmp/messages.pb.h ../src/monclient/messages.pb.h
            mv /tmp/messages.pb.cc ../src/monclient/messages.pb.cc
        fi
    fi
else
    mkdir -p ../monitor/msg
    if [ ! -f ../monitor/msg/messages.pb.go ]; then
        protoc --gogo_out=../monitor/msg/ messages.proto
    else
        protoc --gogo_out=/tmp messages.proto
        if ! cmp -s ../monitor/msg/messages.pb.go /tmp/messages.pb.go; then
            mv /tmp/messages.pb.go ../monitor/msg/messages.pb.go
        fi
    fi
fi
