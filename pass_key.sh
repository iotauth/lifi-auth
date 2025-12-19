#!/bin/bash
# script to build and run the key passer tool
set -e

echo "Building passKeyToPico..."
mkdir -p build
cd build
cmake -DBUILD_TARGET=pi4 .. 
make passKeyToPico

echo "Running passKeyToPico..."
# We are currently in the 'build' directory due to 'cd build' above
# The executable is located in build/receiver/passKeyToPico
./receiver/passKeyToPico
