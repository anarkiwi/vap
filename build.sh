#!/bin/sh

# The llvm-mos SDK and c1541 run from containers (see Makefile), so a container
# runtime is the only build dependency.
set -e

make
