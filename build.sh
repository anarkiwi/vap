#!/bin/sh

export DEBIAN_FRONTEND=noninteractive && \
echo 'debconf debconf/frontend select Noninteractive' | sudo debconf-set-selections
sudo apt-get update && \
sudo apt-get install -yq --no-install-recommends git vice && \
git clone -b V2.19 https://github.com/cc65/cc65 && \
cd cc65 && make && sudo PREFIX=/usr/local make install && cd .. && \
make
