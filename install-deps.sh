#!/bin/bash
set -euo pipefail

echo "installing dependencies"

if [[ $EUID -ne 0 ]]; then
  echo "This script should be run as root."
  exit 1
fi

if [ -f "/etc/os-release" ]; then
  . /etc/os-release
elif [ -f "/etc/arch-release" ]; then
  export ID=arch
  export ID_LIKE=arch
else
  echo "/etc/os-release missing."
  exit 1
fi

deb_deps=(
  autoconf
  automake
  build-essential
  ca-certificates
  cmake
  curl
  git
  golang
  iproute2
  jq
  libabsl-dev
  libaio-dev
  libboost-all-dev
  libibverbs-dev
  libnl-3-dev
  libnl-route-3-dev
  libnuma-dev
  librdmacm-dev
  libssl-dev
  libtool
  make
  meson
  netcat-openbsd
  ninja-build
  pandoc
  patchelf
  pkg-config
  protobuf-compiler
  libprotobuf-dev
  python3-pip
  python3-pyelftools
  rdma-core
  uuid-dev
  uuid-runtime
)

rpm_deps=(
  abseil-cpp-devel
  autoconf
  automake
  boost-devel
  ca-certificates
  cmake
  gcc
  gcc-c++
  git
  golang
  iproute
  jq
  libaio-devel
  libnl3-devel
  libtool
  make
  meson
  ninja-build
  nmap
  numactl-devel
  openssl-devel
  patchelf
  pkgconf
  protobuf-compiler
  protobuf-devel
  python3-pip
  python3-pyelftools
  rdma-core-devel
  util-linux
)

detect_family() {
  local token

  for token in "$ID" ${ID_LIKE:-}; do
    case "$token" in
      ubuntu|debian)
        echo "deb"
        return 0
        ;;
      centos|rhel|fedora|openeuler|openEuler|suse|culinux|cuos)
        echo "rpm"
        return 0
        ;;
      arch)
        echo "arch"
        return 0
        ;;
    esac
  done

  echo "unknown"
}

choose_rpm_installer() {
  if command -v dnf >/dev/null 2>&1; then
    echo "dnf"
    return 0
  fi

  if command -v yum >/dev/null 2>&1; then
    echo "yum"
    return 0
  fi

  return 1
}

ensure_go_on_path() {
  if command -v go >/dev/null 2>&1; then
    return 0
  fi

  if [ -x /usr/lib/golang/bin/go ]; then
    ln -sf /usr/lib/golang/bin/go /usr/bin/go
  fi

  if [ -x /usr/lib/golang/bin/gofmt ] && [ ! -e /usr/bin/gofmt ]; then
    ln -sf /usr/lib/golang/bin/gofmt /usr/bin/gofmt
  fi
}

family=$(detect_family)

case "$family" in
  deb)
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y "${deb_deps[@]}"
    ensure_go_on_path
    ;;
  rpm)
    rpm_installer=$(choose_rpm_installer)
    if [ -z "${rpm_installer:-}" ]; then
      echo "dnf/yum not found for rpm-based distro $ID"
      exit 1
    fi

    "$rpm_installer" install -y "${rpm_deps[@]}"
    ensure_go_on_path
    ;;
  arch)
    echo "Arch Linux is not supported by this script yet."
    exit 1
    ;;
  *)
    echo "Please help us make the script better by sending patches with your OS $ID"
    exit 1
    ;;
esac

echo "dependency installation completed"
