#!/bin/bash

# Navigate to examples script directory
cd deps/iotauth/examples/

# Clean previous artifacts
echo "Cleaning previous artifacts..."
./cleanAll.sh

# Generate new artifacts with password "password"
echo "Generating artifacts..."
./generateAll.sh -p password

# Navigate to auth-server directory
cd ../auth/auth-server/

# Build java project
echo "Building Auth Server..."
mvn clean install

# Run the server
echo "Starting Auth Server..."
java -jar target/auth-server-jar-with-dependencies.jar -p ../properties/exampleAuth101.properties -s password
