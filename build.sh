#!/bin/sh

export DEBIAN_FRONTEND=noninteractive && \
echo 'debconf debconf/frontend select Noninteractive' | sudo debconf-set-selections
sudo apt-get update && \
sudo apt-get install -yq --no-install-recommends vice sudo wget && \
wget -q -O- https://github.com/llvm-mos/llvm-mos-sdk/releases/latest/download/llvm-mos-linux.tar.xz | tar -Jxvf - && \
sudo mv llvm-mos /usr/local && \
make
