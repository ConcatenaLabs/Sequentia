# Standalone faucet page for sequentiatestnet.com/faucet

A single static file, [index.html](index.html), that gives the existing faucet a public
page of its own: users paste any Sequentia testnet address (desktop node, Ambra, web
wallet, …) and request tSEQ / USDX / EURX / GOLD / SILVR / OILX without having to go
through the web wallet first and forward the coins from there.

It is a pure frontend for the faucet backend that is already live: it calls
`POST /faucet` with `{"address": "...", "asset": "USDX"}` (no `asset` key for tSEQ)
and renders the `{txid, amount, asset}` / `{error}` responses, with explorer links to
`/explorer/tx/<txid>`. Same-origin only — no backend changes and no new service needed.
Styling matches the landing page (same palette, cards, fonts).

Tested end-to-end on 2026-07-12 against the live backend (tSEQ, USDX, EURX sends and
the invalid-address error path).

## Deploy

The page only assumes: it is served at `GET /faucet` (or `/faucet/`), and the API stays
at `POST /faucet`. Today `GET /faucet` falls through to the landing page, so e.g.:

```nginx
# GET /faucet -> static page; POST /faucet -> existing faucet backend (unchanged)
location = /faucet {
    if ($request_method = POST) { proxy_pass http://<existing faucet backend>; }
    default_type text/html;
    alias /var/www/faucet/index.html;   # wherever index.html is dropped
}
```

or serve it as a directory index at `/faucet/` and keep `location = /faucet` for POST —
whatever fits the current config. The fetch in the page uses the absolute path
`/faucet`, so both layouts work.

## Homepage card

Suggested card for the landing page grid, before or after "Downloads":

```html
<a class="card" href="/faucet"><h2>Faucet →</h2><p>Free testnet coins: SEQ and sample
assets (USDX, EURX, GOLD, SILVR, OILX), sent straight to any address — full node,
desktop, mobile or web wallet.</p></a>
```

## Two backend notes (nothing blocking)

1. **Error wording**: on an invalid address the backend replies
   `Invalid Bitcoin address: …` — now that the message is shown outside the web wallet
   too, `Invalid Sequentia address` would fit the de-bitcoinization pass better.

2. **Faucet wallet stuck on unconfirmed parents (2026-07-12)**: during today's
   testnet4 block-storm split (same class as the 2026-07-11 incident, fork at
   Sequentia height 16498, gateway bitcoind on `…7458a1` vs public `…5d19f0` at
   testnet4 height 143859), faucet sends returned txids whose *inputs* are themselves
   unconfirmed txs (e.g. faucet tx `00ab4891…` spends `c293c25f…:1`, both
   `confirmed:false`, gone from the mempool while blocks 16516-16522 carry coinbase
   only). The faucet wallet is chaining change off txs that never confirmed after the
   reorgs; once the chains converge it probably needs a rebroadcast/abandon pass (or a
   fresh consolidation to itself) so payouts confirm again. This affects the web
   wallet's built-in faucet today as well — independent of this page.
