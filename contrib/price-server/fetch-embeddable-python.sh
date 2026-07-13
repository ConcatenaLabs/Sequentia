#!/bin/bash
# Fetch the official Windows "embeddable" CPython into ./python/ so the Windows
# installer can bundle a self-contained interpreter for the price-server sidecar
# (the node GUI's Settings -> Price server launcher runs price_server.py with it).
#
# The runtime is NOT vendored in git: it is ~15 MB of Windows binaries that every
# clone would otherwise carry, and price_server.py needs only the standard library
# (see its imports), so the stock embeddable distribution is sufficient as-is.
#
# Run this once before building the Windows installer (make deploy). It is
# idempotent: it re-downloads only if ./python/python.exe is missing.
set -euo pipefail

# Pinned release: verify the archive hash before trusting it (same trust model
# as depends/ source downloads). Bump all three together to move Python version.
PY_VERSION="3.11.9"
PY_URL="https://www.python.org/ftp/python/${PY_VERSION}/python-${PY_VERSION}-embed-amd64.zip"
PY_SHA256="009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dest="${here}/python"

if [ -x "${dest}/python.exe" ] || [ -f "${dest}/python.exe" ]; then
    echo "embeddable Python already present at ${dest} — nothing to do"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
archive="${tmp}/python-embed.zip"

echo "Downloading ${PY_URL}"
curl -fsSL -o "$archive" "$PY_URL"

echo "Verifying SHA256"
actual="$(sha256sum "$archive" | awk '{print $1}')"
if [ "$actual" != "$PY_SHA256" ]; then
    echo "ERROR: checksum mismatch for python-${PY_VERSION}-embed-amd64.zip" >&2
    echo "  expected ${PY_SHA256}" >&2
    echo "  got      ${actual}" >&2
    exit 1
fi

echo "Unpacking into ${dest}"
mkdir -p "$dest"
if command -v unzip >/dev/null 2>&1; then
    unzip -q -o "$archive" -d "$dest"
else
    python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$archive" "$dest"
fi

echo "Done: $(ls "${dest}/python.exe" >/dev/null 2>&1 && echo "python.exe ready" || echo "WARNING python.exe missing")"
