// Production server for the public explorer: serves the static build in
// esplora/dist (Sequentia at /, Bitcoin testnet4 at /testnet4/) and proxies the
// REST API to the local electrs instances. Run behind a Tailscale Funnel (or any
// TLS terminator) pointed at $PORT. No build tooling at runtime.
//
//   SEQ_ELECTRS=127.0.0.1:3003 T4_ELECTRS=127.0.0.1:3004 PORT=8080 \
//     DOWNLOAD_DIR=/path/to/release/artifacts node serve-public.js
//
// NOTE: requires Express 4 (the SPA '*' route below uses the v4 path syntax;
// Express 5 changed wildcard handling). See explorer/package.json.
const express = require('express')
const http = require('http')
const path = require('path')

const DIST = path.join(__dirname, 'esplora', 'dist')
const SEQ_ELECTRS = process.env.SEQ_ELECTRS || '127.0.0.1:3003'
const T4_ELECTRS = process.env.T4_ELECTRS || '127.0.0.1:3004'
const PORT = process.env.PORT || 8080
// Optional release-artifact downloads served at /download (Linux tarball,
// Windows installer, landing page). Defaults to ./downloads next to this file.
const DOWNLOAD_DIR = process.env.DOWNLOAD_DIR || path.join(__dirname, 'downloads')
// The SWK WebAssembly browser wallet served at /wallet (index.html + built
// pkg/). Defaults to ./wallet next to this file.
const WALLET_DIR = process.env.WALLET_DIR || path.join(__dirname, 'wallet')

// Testnet faucet: POST /faucet {address} sends FAUCET_AMOUNT tSEQ from a funded
// node wallet to the address. Propagation to the block producers is handled at
// the node level (not here). Rate-limited per address + per IP.
const { execFile } = require('child_process')
const FAUCET_CLI = process.env.FAUCET_CLI || '/root/SequentiaByClaude/src/elements-cli'
const FAUCET_DATADIR = process.env.FAUCET_DATADIR || '/root/seq-testnet/node-gw'
const FAUCET_WALLET = process.env.FAUCET_WALLET || 'livetest'
const FAUCET_AMOUNT = process.env.FAUCET_AMOUNT || '50000'
const FAUCET_COOLDOWN_MS = Number(process.env.FAUCET_COOLDOWN_MS || 3600000)
const FAUCET_ADDR_RE = /^(tb1|tsqb1)[ac-hj-np-z02-9]{20,180}$/   // bech32/blech32 data charset
const faucetSeen = new Map()                                    // key -> last-served epoch ms
const faucetTooSoon = k => { const t = faucetSeen.get(k); return t && (Date.now() - t) < FAUCET_COOLDOWN_MS }

const proxyTo = target => {
  const [host, port] = target.split(':')
  return (req, res) => {
    const headers = { ...req.headers, host: target }
    delete headers['accept-encoding']
    const up = http.request(
      { host, port: port || 80, method: req.method, path: req.url || '/', headers },
      r => { res.writeHead(r.statusCode, r.headers); r.pipe(res) }
    )
    up.on('error', e => { if (!res.headersSent) res.status(502); res.end('electrs proxy error: ' + e.message) })
    req.pipe(up)
  }
}

const app = express()
app.disable('x-powered-by')

// API proxies first (the /testnet4 prefix is stripped by the mount, so the
// upstream electrs sees /blocks/... etc). Order matters: /testnet4/api before /api.
app.use('/testnet4/api', proxyTo(T4_ELECTRS))
app.use('/api', proxyTo(SEQ_ELECTRS))

// Release-artifact downloads + landing page (before the SPA fallback so
// /download/* is served from DOWNLOAD_DIR, not the esplora index.html).
app.use('/download', express.static(DOWNLOAD_DIR))

// SWK browser wallet (static page + WebAssembly pkg/; express serves .wasm with
// the application/wasm MIME). Before the SPA fallback so /wallet/* is its own.
app.use('/wallet', express.static(WALLET_DIR))

// Testnet faucet. execFile (no shell) + a strict address regex means the
// user-supplied address can't inject anything; it's only ever an argv element.
app.post('/faucet', express.json({ limit: '4kb' }), (req, res) => {
  const address = String((req.body && req.body.address) || '').trim()
  if (!FAUCET_ADDR_RE.test(address)) return res.status(400).json({ error: 'Enter a valid Sequentia address.' })
  const ip = String(req.headers['x-forwarded-for'] || req.socket.remoteAddress || '').split(',')[0].trim()
  if (faucetTooSoon('a:' + address) || faucetTooSoon('i:' + ip))
    return res.status(429).json({ error: 'Already funded recently — please wait before requesting again.' })
  execFile(FAUCET_CLI,
    ['-datadir=' + FAUCET_DATADIR, '-rpcwallet=' + FAUCET_WALLET, '-named', 'sendtoaddress',
      'address=' + address, 'amount=' + FAUCET_AMOUNT, 'fee_rate=2'],
    { timeout: 30000 },
    (err, stdout, stderr) => {
      if (err) return res.status(502).json({ error: String(stderr || err.message).trim().split('\n').pop() || 'faucet send failed' })
      faucetSeen.set('a:' + address, Date.now()); faucetSeen.set('i:' + ip, Date.now())
      res.json({ txid: stdout.trim(), amount: FAUCET_AMOUNT })
    })
})

// Static assets (serves dist/** including dist/testnet4/**).
app.use(express.static(DIST))

// SPA fallbacks: client-side routes (e.g. /block/<hash>) -> the right index.html.
app.get('/testnet4/*', (req, res) => res.sendFile(path.join(DIST, 'testnet4', 'index.html')))
app.get('*', (req, res) => res.sendFile(path.join(DIST, 'index.html')))

app.listen(PORT, () =>
  console.log(`explorer (static+proxy) on :${PORT}  /api->${SEQ_ELECTRS}  /testnet4/api->${T4_ELECTRS}  /download->${DOWNLOAD_DIR}  /wallet->${WALLET_DIR}  /faucet->${FAUCET_AMOUNT} tSEQ from ${FAUCET_WALLET}`))
