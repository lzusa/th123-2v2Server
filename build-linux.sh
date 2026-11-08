#!/bin/sh
set -eu
cd "$(dirname "$0")/native"
g++ -std=c++20 -O2 -DSOKULIB_NO_MOD -I SokuLib/src -I src/Common src/Server/main.cpp src/Server/Server.cpp src/Common/CustomPackets.cpp SokuLib/src/Packet.cpp -o 2v2Server-test8 -lsfml-network -lsfml-system
