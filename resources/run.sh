#!/bin/sh
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:lib:/usr/local/lib:$(pwd)
export LD_LIBRARY_PATH
export LC_ALL="en_US.UTF-8"
cd "$(dirname "$0")"
./creepMiner
