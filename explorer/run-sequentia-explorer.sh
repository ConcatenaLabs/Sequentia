#!/bin/bash
# Launch the Sequentia esplora dev-server pointed at the local electrs.
#   electrs REST must be running on $API_URL (default 127.0.0.1:3003).
set -e
cd "$(dirname "$0")/esplora"
source flavors/sequentia-testnet/config.env
export API_URL="${API_URL:-http://127.0.0.1:3003}"
export PORT="${PORT:-5001}"
echo "Sequentia explorer: web :$PORT  ->  API $API_URL  (asset $NATIVE_ASSET_LABEL)"
exec npm run dev-server
