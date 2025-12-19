#!/bin/bash
# script to build and run the key passer tool
set -e

echo "Building passKeyToPico..."
cd receiver/build
cmake .. 
make passKeyToPico

echo "Running passKeyToPico..."
./passKeyToPico
