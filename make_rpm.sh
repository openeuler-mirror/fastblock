#!/usr/bin/env bash

rpm_version=$1
if [[ -z $rpm_version ]]; then rpm_version="v0.1"; fi

trap 'onCtrlC' INT
function onCtrlC () {
    stty echo

    echo 'Ctrl+C is captured'
    exit 1

}

echo "1.start to install the rpm tools"
rpm_deps=(
  rpm-build
  rpmdevtools
  golang
  dnf-plugins-core
)
yum install -y "${rpm_deps[@]}"
rpmdev-setuptree


echo "2.start to output proto"
root="$(cd $(dirname ${BASH_SOURCE[0]}) && pwd)"
export GOPROXY=https://proxy.golang.com.cn,direct
go install github.com/gogo/protobuf/protoc-gen-gogo@v1.3.2
cp $(go env GOPATH)/bin/protoc-gen-gogo /usr/bin/
cd $root/proto && ./build.sh -t golang
cd $root/proto && ./build.sh -t cpp
cd $root/src/msg/demo && ./gen.sh


echo "3.start to pack and compression fastblock"
cd $root && git status >> /dev/null 2>&1
if [[ $? -ne 0 ]]; then
  commit_version=$rpm_version
else
  git_version=$rpm_version.$(git rev-parse --short HEAD)
  commit_version=$git_version.`git log --oneline|wc -l`
fi
cd $root && sed "s/@VERSION@/$commit_version/g" fastblock.spec.in > fastblock.spec
cd $root && mv fastblock.spec $(rpm --eval %{_specdir})
cd $root/../ && tar -zcvf fastblock-$commit_version.tar.gz fastblock >/dev/null 2>&1
cd $root/../ && mv fastblock-$commit_version.tar.gz $(rpm --eval %{_sourcedir})


echo "4.start to make rpm about fastblock"
go env -w GO111MODULE=""
cd $(rpm --eval %{_specdir}) && yum-builddep fastblock.spec && rpmbuild -ba fastblock.spec
if [[ $? -ne 0 ]];then
  echo "rpm build failed."
  exit -1
fi

echo "rpm build successfully."

exit 0
