#!/bin/bash
set -e

echo "installing dependencies"

if [[ $EUID -ne 0 ]]; then
  echo "This script should be run as root."
  exit 1
fi
if [ -f "/etc/os-release" ]; then
  . /etc/os-release
elif [ -f "/etc/arch-release" ]; then
  export ID=arch
else
  echo "/etc/os-release missing."
  exit 1
fi

deb_deps=(
  pkg-config
  git
  golang
  autoconf
  automake
  pandoc
  libboost-all-dev
  build-essential
  protobuf-compiler
  libprotobuf23
  libprotobuf-dev
  cmake
  make
  jq
  libnl-3-dev
  libnl-route-3-dev
  libibverbs-dev
  librdmacm-dev
  meson
  python3-pyelftools
  uuid-dev
  libssl-dev
  libaio-dev
  ninja-build
  libubsan1
  libasan6
  libboost-dev
  libnuma-dev
)

# Add support for centos/rhel/openEuler/suse

rpm_deps=(
  pkgconfig
  git
  golang
  autoconf
  automake
  gcc
  jq
  gcc-c++
  protobuf-devel
  cmake
  make
  libnl3-devel
  libibverbs-devel
  librdmacm-devel
  meson
  python3-pyelftools
  nc
  libuuid-devel
  openssl-devel
  libaio-devel
  ninja-build
  libubsan
  libasan
  boost-devel
  boost
  numactl-devel
)

case "$ID" in
  ubuntu | debian)
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y "${deb_deps[@]}"
    ;;
  centos | rhel | openEuler | suse | culinux)
    yum install -y "${rpm_deps[@]}"
    ;;
  *)
    echo "Please help us make the script better by sending patches with your OS $ID"
    exit 1
    ;;
esac
