#!/bin/sh

export DEBIAN_FRONTEND=noninteractive && \
echo 'debconf debconf/frontend select Noninteractive' | sudo debconf-set-selections
sudo apt-get update && \
sudo apt-get install -yq --no-install-recommends git vice sudo wget
wget -q -O- https://github.com/llvm-mos/llvm-mos-sdk/releases/download/v15.3.0/llvm-mos-linux.tar.xz | tar -Jxvf - && sudo mv llvm-mos /usr/local && \
make
