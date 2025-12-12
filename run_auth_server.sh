# 
# NOT WORKING (make this work later if wanted)
#!/usr/bin/env bash
#
set -e

# Path to the Auth Server JAR
JAR="deps/iotauth/auth/auth-server/target/auth-server-jar-with-dependencies.jar"
# Path to the properties file for Auth101
PROPS="deps/iotauth/auth/properties/exampleAuth101.properties"

if [[ ! -f "$JAR" ]]; then
    echo "Error: Auth Server JAR not found at $JAR"
    echo "Please run: cd deps/iotauth/auth/auth-server && mvn clean install"
    exit 1
fi

if [[ ! -f "$PROPS" ]]; then
    echo "Error: Properties file not found at $PROPS"
    exit 1
fi

echo "=== Starting Auth Server (Auth101) ==="
echo "Note: This must be running for sender_host to connect."
java -jar "$JAR" -p "$PROPS"
