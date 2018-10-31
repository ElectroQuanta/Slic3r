#!/bin/bash
# Debug/Release/RelWithDebInfo
BUILD=RelWithDebInfo
#BUILD=Debug
rm CMakeCache.txt
cmake -DCMAKE_BUILD_TYPE:STRING=$BUILD ../src
#time cmake --build . -- -j4 -- -DCMAKE_BUILD_TYPE=RelwithDebInfo
time cmake --build . -- -j4
