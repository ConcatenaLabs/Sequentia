# Public deployment (production build + Tailscale Funnel)

Serves both explorers under one origin — `/` (Sequentia) and `/testnet4/`
(Bitcoin testnet4) — as a static build behind a small static+proxy server,
exposed publicly via Tailscale Funnel. The two electrs backends and the server
run as `systemctl --user` services so they survive logout.

## Build + run
1. **Build** the static site (both flavors, relative links): `explorer/build-public.sh`
   → `explorer/esplora/dist/` (`/` + `/testnet4/`). Re-run after any frontend change.
2. **Serve**: `explorer/serve-public.js` serves `dist/` and proxies `/api` →
   electrs `:3003`, `/testnet4/api` → electrs `:3004` (one port, default `:8080`).
3. **Backends**: `run-electrs-supervised.sh` (Sequentia, :3003) and
   `run-electrs-testnet4.sh` (Bitcoin testnet4, :3004). The testnet4 binary +
   16 GB index live under `~/.local/share/concatena-explorer/` (persistent).

## systemd user services (`deploy/systemd/*.service`)
Reference units (absolute paths — adjust if relocating). Install:
```
cp deploy/systemd/concatena-*.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now concatena-electrs-seq concatena-electrs-t4 concatena-explorer
```
- `concatena-electrs-seq` runs the crash-wiping supervisor (the chain=test node churns).
- They auto-start at **login**; for **boot** start: `sudo loginctl enable-linger $USER`.

## Public exposure (Tailscale Funnel)
One-time: `sudo tailscale set --operator=$USER` (so funnel needs no sudo).
Then: `tailscale funnel --bg --https=8443 8080` → `https://<host>.<tailnet>.ts.net:8443`.
(Port 8443 chosen so it doesn't clash with an existing `443` serve. Funnel must
be enabled for the tailnet in the admin console.)

> The **nodes** (`bitcoind` testnet4, the Sequentia cluster) are NOT managed here
> — they must be running for their electrs to serve. They survive logout but not
> a reboot unless separately serviced.
