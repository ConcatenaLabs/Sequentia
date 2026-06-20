#!/bin/bash
# Launch the Bitcoin Testnet4 (parent chain) esplora dev-server, pointed at the
# testnet4 electrs. Pairs with run-sequentia-explorer.sh; the network switcher
# toggles between the two.
set -e
cd "$(dirname "$0")/esplora"
source flavors/bitcoin-testnet4/config.env
export API_URL="${API_URL:-http://127.0.0.1:3004}"
export PORT="${PORT:-5002}"
export BASE_HREF='/'
export MENU_ACTIVE='Bitcoin Testnet4'
export MENU_ITEMS=${MENU_ITEMS:-'{"Sequentia Testnet":"http://127.0.0.1:5001/","Bitcoin Testnet4":"http://127.0.0.1:5002/"}'}
# Reuse the Sequentia network-switcher styling, then recolor the chrome orange.
export CUSTOM_CSS="$CUSTOM_CSS flavors/sequentia-testnet/extras.css flavors/sequentia-testnet/parent-accent.css"
echo "Testnet4 (parent) explorer: web :$PORT  ->  API $API_URL"
exec npm run dev-server
