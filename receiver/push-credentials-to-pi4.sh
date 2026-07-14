#!/bin/bash
#
# Pushes the laptop's current Auth101EntityCert.pem and Net1.ClientKey.pem
# to the Pi4's own clone of this repo, so the Pi4 stays in sync whenever
# the Auth server's credentials are regenerated on the laptop.
#
# USAGE: run from the repo root, after ensuring the Auth server has
#        generated the latest credentials (and after running
#        receiver/update-credentials.sh if you also test locally).
#        $ ./receiver/push-credentials-to-pi4.sh [pi4-host-alias]
#        (defaults to pi4-home; pass "pi4" to use the hotspot alias instead)

set -e

PI4_HOST="${1:-pi4-home}"
PI4_REPO_DIR="~/projects/lifi-auth"

AUTH_CERT_PATH="deps/iotauth/entity/auth_certs/Auth101EntityCert.pem"
CLIENT_KEY_PATH="deps/iotauth/entity/credentials/keys/net1/Net1.ClientKey.pem"

if [ ! -f "${AUTH_CERT_PATH}" ] || [ ! -f "${CLIENT_KEY_PATH}" ]; then
    echo "Error: run this from the repo root (lifi-auth), credential files not found."
    exit 1
fi

# Open one authenticated connection (single password prompt) and share it
# across both scp calls via a temporary control socket.
CTRL_SOCK="/tmp/pi4-ctrl-$$"
cleanup() { ssh -S "${CTRL_SOCK}" -O exit "${PI4_HOST}" 2>/dev/null || true; }
trap cleanup EXIT

echo "Connecting to ${PI4_HOST}..."
ssh -M -S "${CTRL_SOCK}" -fN "${PI4_HOST}"

echo "Pushing credentials to ${PI4_HOST}:${PI4_REPO_DIR} ..."
scp -o ControlPath="${CTRL_SOCK}" "${AUTH_CERT_PATH}" "${PI4_HOST}:${PI4_REPO_DIR}/${AUTH_CERT_PATH}"
scp -o ControlPath="${CTRL_SOCK}" "${CLIENT_KEY_PATH}" "${PI4_HOST}:${PI4_REPO_DIR}/${CLIENT_KEY_PATH}"

echo "Done. Verify on the Pi4 with:"
echo "  openssl x509 -in ${AUTH_CERT_PATH} -noout -serial -dates"
