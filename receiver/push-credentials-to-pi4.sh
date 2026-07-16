#!/bin/bash
#
# Pushes the laptop's current Auth101EntityCert.pem/Net1.ClientKey.pem and
# Auth102EntityCert.pem/Net2.ClientKey.pem to the Pi4's own clone of this
# repo, so the Pi4 stays in sync whenever the Auth server's credentials are
# regenerated on the laptop.
#
# USAGE: run from the repo root, after ensuring the Auth server(s) have
#        generated the latest credentials (and after running
#        receiver/update-credentials.sh if you also test locally).
#        $ ./receiver/push-credentials-to-pi4.sh [pi4-host-alias]
#        (defaults to pi4-home; pass "pi4" to use the hotspot alias instead)

set -e

PI4_HOST="${1:-pi4-home}"
PI4_REPO_DIR="~/projects/lifi-auth"

FILES=(
    "deps/iotauth/entity/auth_certs/Auth101EntityCert.pem"
    "deps/iotauth/entity/credentials/keys/net1/Net1.ClientKey.pem"
    "deps/iotauth/entity/auth_certs/Auth102EntityCert.pem"
    "deps/iotauth/entity/credentials/keys/net2/Net2.ClientKey.pem"
)

for f in "${FILES[@]}"; do
    if [ ! -f "${f}" ]; then
        echo "Error: run this from the repo root (lifi-auth), credential file not found: ${f}"
        exit 1
    fi
done

# Open one authenticated connection (single password prompt) and share it
# across all scp calls via a temporary control socket.
CTRL_SOCK="/tmp/pi4-ctrl-$$"
cleanup() { ssh -S "${CTRL_SOCK}" -O exit "${PI4_HOST}" 2>/dev/null || true; }
trap cleanup EXIT

echo "Connecting to ${PI4_HOST}..."
ssh -M -S "${CTRL_SOCK}" -fN "${PI4_HOST}"

echo "Pushing credentials to ${PI4_HOST}:${PI4_REPO_DIR} ..."
for f in "${FILES[@]}"; do
    scp -o ControlPath="${CTRL_SOCK}" "${f}" "${PI4_HOST}:${PI4_REPO_DIR}/${f}"
done

echo "Done. Verify on the Pi4 with, e.g.:"
echo "  openssl x509 -in deps/iotauth/entity/auth_certs/Auth102EntityCert.pem -noout -serial -dates"
