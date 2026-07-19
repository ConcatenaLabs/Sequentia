# Issuing an asset, step by step

This guide is for people who are not web developers. It assumes you have a
website and can log in to whoever hosts it. Nothing here requires you to
understand cryptography.

**What you are about to do.** Creating an asset takes ten seconds. Making it show
its name in other people's wallets takes three more steps, because wallets refuse
to display a name unless the person who owns the website says the asset is
theirs. That is the whole point: it stops someone else from issuing a token,
calling it yours, and fooling your customers.

**The one thing you cannot undo.** The name, the ticker and your domain get baked
into the asset's identity the moment you create it. Not stored next to it —
*baked in*. If you get the domain wrong, there is no edit button, no support
line, no fix. The only way out is to throw the asset away and make another one.
Everything else in this guide is easy. Take your time on the domain.

---

## Step 1 — Find your exact domain

Websites often answer to two names: `example.com` and `www.example.com`. They
look the same to you, but they are two different names, and only **one** of them
is the right answer here.

You do not need to know which. Your browser will tell you.

1. Open a new browser tab.
2. Type your website's address and press Enter.
3. **Wait for the page to finish loading.**
4. Now look at the address bar. Read what is actually written there.

Whatever the address bar says *after* the page has loaded is your domain. If you
typed one thing and the address bar now shows another, your website moved you —
and the name it moved you *to* is the one that works.

Write down just the name. No `https://`, no slash, nothing after the first slash.

### Two examples

**Alberto's site.** He types `albertodeluigi.com`. The page loads and the address
bar reads `https://www.albertodeluigi.com/`. His website moved him to the `www`
version, so his domain is:

```
www.albertodeluigi.com
```

**A site that does the opposite.** Someone types `www.example.org` and the address
bar settles on `https://example.org/`. Theirs is:

```
example.org
```

### Why this matters so much

The check that proves the domain is yours is done by a machine, and that machine
is deliberately literal: it asks your website for one exact address and accepts
only a direct answer. If your site replies "that's not me, go to the www version
instead", the machine does not follow — it just records a failure. Your asset
stays nameless, permanently.

A human would shrug at the difference between `example.com` and
`www.example.com`. The machine cannot, because being relaxed about which website
answered is exactly how impersonation gets in.

### Getting it into Sequentia Core

Type it, or paste it, into **Your domain** on the Assets page. Pasting the whole
address from the browser is fine — Core trims `https://` and anything after the
first slash for you. These all end up as `www.albertodeluigi.com`:

| What you paste | What Core uses |
|---|---|
| `https://www.albertodeluigi.com/` | `www.albertodeluigi.com` |
| `www.albertodeluigi.com` | `www.albertodeluigi.com` |
| `https://www.albertodeluigi.com/blog/` | `www.albertodeluigi.com` |
| `WWW.AlbertoDeLuigi.COM` | `www.albertodeluigi.com` |

Core checks that the name exists on the internet before it lets you spend
anything, and warns you if it cannot find it — usually a typo. It cannot check
the `www` question for you, which is why Step 1 is your job. The **Open my site**
button next to the field runs the address-bar test for you: it opens the domain
exactly as you typed it, so you can see with your own eyes where you land.

---

## Step 2 — Fill in the rest

**Name** — what people read. `Alberto De Luigi Token`. Spaces and accents are
fine.

**Ticker** — the short symbol, like `GOLD` or `ADL`. Up to 12 characters, letters
and digits. First come, first served across the whole network: if someone
registered `ADL` before you, yours will be refused later, and by then it is too
late to change. Pick something unlikely to collide.

**Decimal places** — how finely the asset splits. `8` means someone can hold
0.00000001 of it, like Bitcoin. `0` means whole units only, good for things that
cannot be halved, like tickets or shares. If unsure, leave `8`.

**Amount of units** — how many you create now.

**Reissuance tokens** — leave `1` to keep the ability to make more later. `0`
means the supply is fixed forever.

---

## Step 3 — Issue it

Press **Issue asset**. Core shows you what it is about to commit to and asks you
to confirm. Read that box properly — it is your last chance.

Afterwards you get an **asset id**: 64 characters of gibberish. That is your
asset's real name as far as the network is concerned. Everything else is
decoration that has to be proven.

Core then shows a panel with a line of text and a **Save the proof file** button.
Save it somewhere you can find. That file is Step 4.

---

## Step 4 — Put the file on your website

The file has a long name and no extension. Do not rename it: the name is what
ties it to your asset.

It has to end up in a folder called `.well-known` at the very top of your site,
so that this address works:

```
https://www.albertodeluigi.com/.well-known/sequentia-asset-proof-<your asset id>
```

The dot at the start of `.well-known` is intentional. It is a standard folder
that many services use for exactly this kind of proof.

Whatever runs your site, the job is the same: get a file into a folder. What
changes is where you find the folder. Below is WordPress in full, then the short
version for the other common platforms.

### On WordPress

**Do not use the Media Library.** Files added there land in a different folder
and WordPress refuses files without an extension anyway. You need to reach the
actual folders of your site, which means one of these:

**Your host's file manager** (easiest). Log in to your hosting control panel —
cPanel, Plesk, or your provider's own dashboard — and find "File Manager". Open
the folder where WordPress lives; it is usually called `public_html`, sometimes
`htdocs` or `www`, and you will recognise it because it contains `wp-content` and
`wp-config.php`. In there:

1. Make a new folder called exactly `.well-known` (if it is not already there —
   it often is).
2. Upload your proof file into it.
3. If the file manager hides the folder after you make it, turn on "show hidden
   files" in its settings. Folders starting with a dot are hidden by convention.

**FTP** — same thing with a program like FileZilla, if your host gave you FTP
details.

**A plugin** — search the plugin directory for a file manager plugin. It gives
you the same folders from inside `wp-admin`. Use one with a good reputation and
remove it when you are done; a plugin that can write anywhere is worth being
fussy about.

You do **not** need to worry about WordPress swallowing the address. WordPress
only handles addresses that do not match a real file, so a real file in
`.well-known` is served straight out.

### On the other common platforms

**Squarespace, Wix, Shopify** — you cannot upload a file to an arbitrary path on
any of them. Their file handling is limited to media, and none will serve
`/.well-known/…` for you. Your options are to use a subdomain you point at
somewhere you do control, or to pick a different domain for the asset. Decide this
**before** issuing, since the domain is permanent.

**Netlify, Vercel, Cloudflare Pages, GitHub Pages** — easy. Put the file in a
`.well-known/` folder inside the directory you publish (`public/`, `static/`, or
the repo root for GitHub Pages) and deploy. GitHub Pages needs a `.nojekyll` file
at the root, or it will skip dotfolders entirely. Netlify and Vercel both let you
set the content type in their config file; on GitHub Pages you cannot, which may
be a dead end for the `text/plain` requirement below.

**A plain server you control (Apache, Nginx, cPanel)** — the WordPress
instructions above are really just this: find the web root, make the folder,
upload.

**Your own Node/Python app** — serve the one route yourself and return
`Content-Type: text/plain`. This is the easiest case; you are not fighting anyone.

### The bit that usually catches people out

The file must be delivered as **plain text**. Because it has no extension, most
web servers will not say what it is, and the check then refuses it — not because
the content is wrong, but because the server never said "this is text".

The fix is a small settings file next to it. In the same `.well-known` folder,
create a file called `.htaccess` (dot first, no extension) containing exactly:

```apache
<Files "sequentia-asset-proof-*">
  ForceType text/plain
</Files>
```

That is Apache, which is what most WordPress hosting runs. If your host uses
Nginx you cannot do it with a file and will have to ask them to add
`default_type text/plain;` for the `/.well-known/` location — or just ask their
support to "serve /.well-known/ files as text/plain", which they will understand.

If you use Cloudflare or any caching service, clear the cache afterwards, or it
may keep serving the old "not found" for a while.

This step is annoying but it is **not permanent**. Unlike the domain, you can get
it wrong, fix it, and try again as many times as you like.

---

## Step 5 — Check it actually worked

Open the proof address in your browser — the whole long one, ending in your asset
id. You should see one line of plain text and nothing else:

```
Authorize linking the domain name www.albertodeluigi.com to the Sequentia asset 3a0f...
```

Go through this list:

- **You see the line, as plain text.** Good.
- **You get "page not found".** The file is in the wrong folder, or its name got
  changed. Watch for a `.txt` your computer added when saving.
- **The address bar changed to a different domain.** Your domain is wrong. This is
  Step 1 again, and if you have already issued the asset it is unfortunately too
  late for that one.
- **The browser downloads the file instead of showing it.** The content-type
  problem above. The line is right, the server just is not calling it text. Fix
  the `.htaccess`.
- **You see your website's design around the line.** The file is being rendered as
  a page. It must be the raw line and nothing else — no theme, no header.

---

## Step 6 — Register it

Publishing the proof does not announce anything by itself. You still have to tell
a registry your asset exists, and it will then go and read your file.

On the Assets page, put your asset id in **Register an asset with the registry**
and press **Register**. Your wallet kept the contract from when you issued, and
sends it along. There is no harm in pressing it too early: the registry checks
your file itself, so if it is not up yet it just tells you so, and you try again.

Nothing here is secret, and nothing needs anyone's approval — the registry decides
on your file and the chain, not on who you are.

It goes to whichever registry your node reads (`-assetregistryurl`, the public one
by default). If you point your node at a different registry, Register follows it
there.

Once it accepts, wallets pick up the name within about five minutes. Core tells
you when it has: the **Registry** column on the Assets page turns from *not
registered yet* into your asset's name.

---

## Questions people actually ask

**Do I need a website for this?**
You need a domain you control and can put a file on. A one-page site is enough. If
you have no domain at all, you can still create the asset — it just stays a hex id
in every wallet, forever.

**Can I use my friend's domain?**
Only if they put the file up. And be clear about what it means: the domain is the
asset's *identity*. An asset pointing at `albertodeluigi.com` is telling the world
Alberto issued it. That is a statement about who is responsible, not about where a
file is parked.

**Can I host proofs for other people's assets on my domain?**
Yes, and the same warning applies in reverse: those assets will show as issued by
you. That is fine among friends who understand it. It is not a neutral hosting
service.

**My domain expired / my site is down. Do I lose the name?**
No. The check happens once, when the registry accepts your asset. After that the
entry stands on its own. A site going down later costs you nothing.

**I made a typo in the name. Can I fix it?**
No. Name, ticker, domain and decimals are all committed into the asset id.
Nothing can change them. Issue a new asset and abandon the old one — on testnet
that costs you nothing but a minute.

**Do I have to use a `www` domain?**
You have to use whatever your website actually answers to. See Step 1. Neither is
better; they are just different names, and only one of them is yours in practice.

**Can I use a subdomain, like `tokens.albertodeluigi.com`?**
Yes, if you can put a file there. Be aware it is a *different* domain from
`www.albertodeluigi.com` as far as this is concerned, and the proof has to live on
the subdomain itself.

**Why won't Core just check the file for me after I issue?**
It cannot fetch secure web pages — it has no ability to speak HTTPS, by design,
which keeps a lot of complexity out of a program that holds your money. It checks
what it can (that the domain exists, that the fields are acceptable) and then
watches the registry, which is what actually decides. That is why Step 5 is done
in your browser.

**Someone registered my ticker.**
Tickers are first come, first served. There is no arbitration. Choose another.

**What is the `contract` thing Core printed?**
The little document holding your asset's name, ticker, decimals, domain and key.
Only its fingerprint goes on the chain, not the document itself — so if you lose
it, nobody can reconstruct it, and you cannot register the asset. Keep a copy.

---

For what is happening underneath — the contract, the fingerprint, how the asset id
is derived, what the registry checks — see
[asset contracts and verification](asset-contracts-and-verification.md).
