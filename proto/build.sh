#!/bin/bash

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
	protoc --cpp_out=../src/rpc common_msg.proto
	protoc --cpp_out=../src/rpc raft_msg.proto
	protoc --cpp_out=../src/rpc osd_msg.proto
	protoc --cpp_out=../src/monclient/ messages.proto
else
	mkdir -p ../monitor/msg
	protoc --gogo_out=../monitor/msg/  messages.proto
fi
