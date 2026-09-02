# Release publisher

Keeps `sequentiatestnet.com/download/` current without anyone touching the box.
It covers **everything the page offers**, not only the node: Sequentia Core, the
Fulmen Lightning wallet, the browser extension, the Ambra mobile wallet,
Seqognito and the Pignus lending CLI.

Every ten minutes it asks each product what version is published upstream. If that
differs from what it published last, it builds that version and puts the artifacts
where the page serves them, then points each card at the newest file that actually
exists.

## Adding a product

Drop one file in `products.d/`. The driver knows nothing about how anything is
built. A recipe defines:

| | |
|---|---|
| `PRODUCT_NAME` | its name |
| `PRODUCT_REPO` | where it lives |
| `PRODUCT_INDEX_GLOB` | the artifact filename patterns on the download page |
| `remote_version()` | what version is published upstream |
| `build(version, outdir)` | put artifacts in `outdir` |
| `requirements()` | optional: print a reason and fail to be skipped |

Anything the recipe writes into `outdir` gets published. Nothing else is needed.

## The products

| Product | Versioned by | Publishes |
|---|---|---|
| `node` | git **tag** | Linux tarball + Windows installer |
| `extension` | `manifest.json` | zip of the repo, minus development files |
| `fulmen` | `package.json` | Linux AppImage + Windows zip + Windows installer |
| `ambra` | `app/pubspec.yaml` | signed Android APK |
| `seqognito` | `package.json` | Linux AppImage + Windows zip + Windows installer |
| `pignus` | `pignus/__init__.py` | source tarball of the repository, minus development files |

Every artifact the download page offers is covered. Nothing is published by hand.

Every run also rewrites `SHA256SUMS` in the download directory over every
artifact present, aliases included, so `sha256sum --ignore-missing -c SHA256SUMS`
verifies a download under whichever name it arrived with. It is regenerated from
the directory, not from the run, for the same reason the index is: a product that
was skipped keeps its previous artifact and that artifact still needs a line.

`SHA256SUMS.asc` is a detached OpenPGP signature over it, and
`sequentia-release-signing-key.asc` beside it is the public key. The signing key
lives only on the box, in `/etc/sequentia/release-signing` (mode 700, passphrase
free so the timer can use it; back it up with the other secrets there) and is
never in any repository. Its fingerprint, the copy to compare against:

```
B4F5 7796 7E32 25D5 8FF6 3DFC 5974 FD59 E609 F11F
```

To verify a download:

```
gpg --import sequentia-release-signing-key.asc      # check the fingerprint above
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum --ignore-missing -c SHA256SUMS
```

A box without the key publishes the checksums unsigned and logs that it did.

`seqognito` is the only recipe that compiles something before packaging: it does
not vendor its wasm signer, so it builds `lwk_wasm` from SWK first and refuses to
package if the result lacks the mixing exports. That is a cold Rust build, but
only when its version actually changed.

The node is versioned by tag and the rest by their version field, and that is
deliberate: `doc/sequentia/release-versioning.md` requires a tag for the node,
because a tag is the only reliable map from the version a node reports on the wire
back to the code it is running. The other products have no such wire identity, and
requiring a tag in four repositories would mean four chances to forget one.

Cutting a node release is therefore:

```
git tag -a v24.1.0 <commit> -m "Sequentia Core 24.1.0"
git push origin v24.1.0
```

For the others, bumping the version field and pushing is enough.

## What it refuses to do

**It will not move a card backwards.** The state file only knows what this script
published; artifacts placed by hand over the years are older history it has never
seen, and one can be *ahead* of what its repository's default branch currently says.
Ambra was exactly that case: the page offered 0.16.4 while `main` still read 0.13.7,
because the work sat on an unmerged branch. The branch has since been merged, but the
guard stays: publishing a lower version would quietly offer users a downgrade, which
for a wallet is worse than offering nothing.

**It will not publish a debug-signed APK.** Such a build installs and can then never
be upgraded by a real release, stranding whoever installed it.

**It will not point a card at a file that is not there.** Cards are rewritten from
what is present, so a product that was skipped or failed keeps offering its previous
version rather than becoming a 404.

**One product's problems stay its own.** A missing toolchain skips that product with
a reason; the others still publish.

## Why it builds where it does

The committee is started from `/root/Sequentia/src/sequentiad`. A release
build in that tree would replace the binary twenty running nodes were started from,
and because a running process keeps its inode the swap would only surface at the next
restart. The publisher builds under `/root/sequentia/release-build` and writes nowhere
else except the download directory.

It is also niced with limited jobs throughout. This box produces blocks, and starving
it is how the testnet stalls.

## One-time setup

```
apt-get install -y g++-mingw-w64-x86-64 nsis bison flex zip
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix

# The publisher's own clone, pinned to master. Kept apart from every product's
# checkout: the node recipe checks out a release TAG, and when the publisher lived
# in that same tree it replaced its own script with the tagged version's.
git clone https://github.com/ConcatenaLabs/Sequentia.git \
  /root/sequentia/release-build/publisher

# The node's build tree, where the depends prefixes live.
git clone https://github.com/ConcatenaLabs/Sequentia.git \
  /root/sequentia/release-build/src/node
cd /root/sequentia/release-build/src/node
nice -n 19 make -C depends HOST=x86_64-pc-linux-gnu -j4   # Linux prefix, incl. Qt
nice -n 19 make -C depends HOST=x86_64-w64-mingw32 -j4    # Windows cross prefix

install -m644 /root/sequentia/release-build/publisher/contrib/sequentia/release-publisher/seq-release-publisher.{service,timer} \
  /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now seq-release-publisher.timer
```

A depends prefix is **not relocatable**: absolute paths are baked into its `.pc`
files and into the libraries themselves. Moving the build tree breaks it, and
rewriting those paths with `sed` corrupts every binary it touches, because a
shorter replacement truncates the file. If the tree has to move, either rebuild
depends from scratch or leave a symlink at the old path.

Ambra additionally needs a Flutter, Android and Rust toolchain:

```
apt-get install -y openjdk-17-jdk-headless
curl -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
rustup target add aarch64-linux-android
cargo install cargo-ndk
git clone -b stable --depth 1 https://github.com/flutter/flutter.git /opt/flutter
# Android SDK + the NDK version pinned in app/android/app/build.gradle.kts
sdkmanager --sdk_root=/opt/android-sdk "platform-tools" "platforms;android-34" \
  "build-tools;34.0.0" "ndk;29.0.14206865"
```

and the release signing key, which is the one thing the box cannot install for
itself:

```
/etc/sequentia/ambra-release.keystore   # mode 600, root
/etc/sequentia/ambra-key.properties     # storeFile= points at the above
```

That key *is* the app's identity: a phone refuses an update signed by a different
one, so it can never be regenerated or rotated without stranding every existing
user. It is copied there by hand, once, and lives nowhere in git.

## Operating it

```
systemctl list-timers seq-release-publisher.timer   # when it next checks
journalctl -u seq-release-publisher.service -f      # follow a build
systemctl start seq-release-publisher.service       # check right now
SEQ_ONLY=node ./publish-release.sh                  # one product
```

State is one file per product under `/root/sequentia/release-build/state/`. Delete one
to force that product to be built and published again.

## Settings

| Variable | Default | |
|---|---|---|
| `SEQ_BUILD_ROOT` | `/root/sequentia/release-build` | clones, state and staging |
| `SEQ_DOWNLOAD_DIR` | `/root/sequentia/downloads` | what the page serves |
| `SEQ_SIGNING_GNUPGHOME` | `/etc/sequentia/release-signing` | the release signing key; absent means unsigned |
| `SEQ_BUILD_JOBS` | `4` | raise only if the committee is not running |
| `SEQ_BUILD_NICE` | `19` | |
| `SEQ_ONLY` | all | space-separated product names |
| `SEQ_NODE_BUILD_WINDOWS` | `1` | `0` publishes the Linux tarball only |
| `SEQ_NODE_CONFIGURE_ARGS` | `--with-gui=qt5 --disable-tests --disable-bench --enable-any-asset-fees` | `--enable-any-asset-fees` is what gives fee amounts their reference units instead of labelling them BTC/sat |
| `SEQ_AMBRA_KEYSTORE` | `/etc/sequentia/ambra-release.keystore` | |

## Failure

A failed build leaves the page untouched: artifacts land via a `.part` file and are
moved into place only once complete, and the version is recorded last, so a failure
retries the same version rather than skipping it.
`systemctl status seq-release-publisher.service` shows it and the journal has the
build output.
