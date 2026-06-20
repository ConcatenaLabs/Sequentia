#!/bin/bash
# Launch the Sequentia esplora dev-server. The dev-server proxies /api -> the
# local electrs ($ELECTRS_HTTP), so the explorer works over any origin (localhost
# or a remote host like Tailscale) without baking the API address into the client.
set -e
cd "$(dirname "$0")/esplora"
source flavors/sequentia-testnet/config.env
export ELECTRS_HTTP="${ELECTRS_HTTP:-127.0.0.1:3003}"   # Sequentia electrs REST
export API_URL="${API_URL:-/api}"                       # same-origin, proxied
export PORT="${PORT:-5001}"
echo "Sequentia explorer: web :$PORT  ->  /api -> $ELECTRS_HTTP  (asset $NATIVE_ASSET_LABEL)"
exec npm run dev-server
