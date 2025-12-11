#!/usr/bin/env bash
set -e

# Ensure we have the SDK path set
export PICO_SDK_PATH="$(pwd)/lib/pico-sdk"

echo "=== Building Sender Host Application ==="
# 'pi4' target invokes the receiver/CMakeLists.txt which defines sender_host
./run_build.sh pi4

# The artifact is symlinked to 'latest'
HOST_APP="./artifacts/pi4/latest"

if [[ ! -x "$HOST_APP" ]]; then
    echo "Error: Could not find executable at $HOST_APP"
    exit 1
fi

echo ""
echo "=== Running Sender Host Application ==="
# Run with the config file
"$HOST_APP" lifi_sender.config
