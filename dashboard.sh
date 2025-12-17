#!/bin/bash
set -e

# Define paths
VENV_DIR="venv"
APP_PATH="sender/dashboard/app.py"
REQ_PATH="sender/dashboard/requirements.txt"

# Check if venv exists
if [ ! -d "$VENV_DIR" ]; then
    echo "Creating virtual environment..."
    python3 -m venv "$VENV_DIR"
fi

# Activate venv
source "$VENV_DIR/bin/activate"

# Check dependencies
echo "Installing/Updating dependencies..."
pip install -r "$REQ_PATH" > /dev/null

# Run App
echo "Starting Sender Dashboard..."
echo "Open your browser to: http://localhost:5000"
echo "------------------------------------------------"
python "$APP_PATH"
