#!/bin/bash

buildtype="Release"
component="osd"
usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -t, --buildtype : can be Release or Debug"
    echo "  -c, --component : can be osd or monitor"
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
        -c|--component)
	    shift
            component="$1"
            ;;
        *)
            echo "Unknown argument: $1"
            usage
            ;;
    esac
    shift
done

echo $buildtype
echo $component

root="$(cd $(dirname ${BASH_SOURCE[0]}) && pwd)"
if [[ -z ${CC} ]]; then export CC=/usr/bin/gcc; fi
if [[ -z ${CXX} ]]; then export CXX=/usr/bin/g++; fi

(cd $root/src/msg/demo && ./gen.sh)


if [ "$component" = "monitor" ]; then
	echo "build fastblock(fastblock-mon、fastblock-client) Golang code"
	export GOPROXY=https://proxy.golang.com.cn,direct

	if [ ! -f "/usr/bin/protoc-gen-gogo" ];then
		echo "we should build protoc-gen-gogo"
		go install github.com/gogo/protobuf/protoc-gen-gogo@v1.3.2
		cp $(go env GOPATH)/bin/protoc-gen-gogo /usr/bin/
	fi
	cd $root/proto && ./build.sh -t golang
	(cd $root/monitor && make)
fi
if [ "$component" = "osd" ]; then
	echo "build fastblock(fastblock-osd、fastblock-vhost) C/C++ code"
	cd $root/proto && ./build.sh -t cpp
	cmake -DCMAKE_BUILD_TYPE=$buildtype \
	  -B$root/build \
	  -H$root \
	  -DCMAKE_C_COMPILER=$CC \
	  -DCMAKE_CXX_COMPILER=$CXX \
	  "$@"
	
	(cd $root/build && make -j `grep -c ^processor /proc/cpuinfo`)
fi


