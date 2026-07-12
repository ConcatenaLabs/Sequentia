# Landing page for sequentiatestnet.com

The root page served at `GET /` on sequentiatestnet.com: a single static
[index.html](index.html) (inline `<style>`, no build step, no external CSS/JS) with the
card grid linking to Explorer, Web Wallet, Faucet, Emissio, Bridge and Downloads.

This file was brought into the repo verbatim from the live site on 2026-07-12 so that
the landing page follows the same GitHub-pull deploy flow as the rest of the site: edit
here, open a PR, merge, and the box pulls from GitHub. Nothing is pushed to the box from
a workstation.

## What changed vs. the previously live page

Added one card — **Faucet →** (`/faucet`) — between "Web Wallet" and "Emissio Rewards".
It points at the standalone faucet page in [../faucet-page/](../faucet-page/). Everything
else (markup, styles, copy, footer) is byte-for-byte the page that was already live.

## Deploy

Serve this file at `GET /` (whatever path the box currently serves the root from), e.g.:

```nginx
location = / {
    default_type text/html;
    alias /var/www/landing/index.html;   # wherever index.html is dropped
}
```

Before this becomes the source of truth, confirm with Andreas that the box's `/` is (or
will be) served from this repo file, so a `git pull` on the box updates the homepage.
The card links (`/explorer/`, `/wallet/`, `/faucet`, `/emissio/`, `/bridge/`,
`/download/`) and the two footer images (`/explorer/img/icons/...`) are all absolute
paths already served by the box — no asset changes needed.
