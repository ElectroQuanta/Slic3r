#!/bin/bash
rm CMakeCache.txt
cmake ../src
time cmake --build . -- -j4
