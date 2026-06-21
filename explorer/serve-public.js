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

// Static assets (serves dist/** including dist/testnet4/**).
app.use(express.static(DIST))

// SPA fallbacks: client-side routes (e.g. /block/<hash>) -> the right index.html.
app.get('/testnet4/*', (req, res) => res.sendFile(path.join(DIST, 'testnet4', 'index.html')))
app.get('*', (req, res) => res.sendFile(path.join(DIST, 'index.html')))

app.listen(PORT, () =>
  console.log(`explorer (static+proxy) on :${PORT}  /api->${SEQ_ELECTRS}  /testnet4/api->${T4_ELECTRS}  /download->${DOWNLOAD_DIR}`))
