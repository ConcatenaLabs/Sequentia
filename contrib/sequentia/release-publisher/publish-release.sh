#!/bin/bash
# Publish a Sequentia Core release to the download page, unattended.
#
# Watches the repository for a new release tag and, when one appears, builds the
# Linux tarball and the Windows installer from that exact tag and publishes both.
# Run it from a timer; it is idempotent, so running it when nothing has changed
# costs one `git ls-remote` and exits.
#
# WHY IT BUILDS FROM A TAG AND NOT FROM master: what the download page offers is
# the one artifact a stranger runs without reading the source, so it has to be a
# thing we named. A tag is also the only way this script can tell "there is a new
# release" from "someone pushed a commit".
#
# WHY IT HAS ITS OWN CLONE: the committee is started from
# /root/SequentiaByClaude/src/sequentiad. A release build in that tree would swap
# the binary 20 running nodes were started from. This one builds in its own clone
# and never writes outside it and the download directory.
#
# The build is deliberately niced and limited to a few jobs: this machine also
# produces blocks, and starving it is how a testnet stalls.
set -euo pipefail

REPO_URL="${SEQ_REPO_URL:-https://github.com/GracedEternalKingCabbageMan/Sequentia.git}"
BUILD_ROOT="${SEQ_BUILD_ROOT:-/root/sequentia/release-build}"
REPO_DIR="${SEQ_REPO_DIR:-$BUILD_ROOT/Sequentia}"
STATE_DIR="${SEQ_STATE_DIR:-$BUILD_ROOT/state}"
DOWNLOAD_DIR="${SEQ_DOWNLOAD_DIR:-/root/sequentia/downloads}"
JOBS="${SEQ_BUILD_JOBS:-4}"
NICE="${SEQ_BUILD_NICE:-19}"
# Set to 0 to publish the Linux tarball only (skips the cross build, which is the
# slow half). The page then keeps advertising the previous Windows installer.
BUILD_WINDOWS="${SEQ_BUILD_WINDOWS:-1}"
CONFIGURE_ARGS="${SEQ_CONFIGURE_ARGS:---disable-tests --disable-bench --enable-any-asset-fees}"

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

mkdir -p "$STATE_DIR" "$BUILD_ROOT"

# One publisher at a time. A second timer tick during a two-hour build must not
# start a second build in the same tree.
exec 9>"$STATE_DIR/publish.lock"
if ! flock -n 9; then
  log "another publish is running; nothing to do"
  exit 0
fi

[ -d "$REPO_DIR/.git" ] || die "no clone at $REPO_DIR (create it once: git clone $REPO_URL $REPO_DIR)"

# --- Is there a new release? ---------------------------------------------------
# Sort by version, not lexically, so v24.0.10 beats v24.0.9.
latest_tag=$(git -C "$REPO_DIR" ls-remote --tags --refs origin 'v[0-9]*' \
  | awk '{print $2}' | sed 's#refs/tags/##' \
  | sort -V | tail -1)
[ -n "$latest_tag" ] || die "no version tags found on origin"

state_file="$STATE_DIR/published-tag"
published=$(cat "$state_file" 2>/dev/null || echo "")

if [ "$latest_tag" = "$published" ]; then
  log "up to date: $latest_tag already published"
  exit 0
fi

version="${latest_tag#v}"
log "new release: $latest_tag (last published: ${published:-none})"

linux_art="sequentia-core-$version-linux-x86_64.tar.gz"
win_art="sequentia-core-$version-win64-setup.exe"

# --- Check out exactly that tag ------------------------------------------------
git -C "$REPO_DIR" fetch -q --tags origin
git -C "$REPO_DIR" reset -q --hard
git -C "$REPO_DIR" clean -qfd -e depends
git -C "$REPO_DIR" checkout -q "refs/tags/$latest_tag"
log "checked out $latest_tag ($(git -C "$REPO_DIR" rev-parse --short HEAD))"

cd "$REPO_DIR"

build() { nice -n "$NICE" "$@"; }

# --- Linux -------------------------------------------------------------------
depends_linux="$REPO_DIR/depends/x86_64-pc-linux-gnu"
[ -d "$depends_linux" ] || die "missing depends prefix $depends_linux (build once: make -C depends HOST=x86_64-pc-linux-gnu)"

log "building Linux ($JOBS jobs, nice $NICE)"
build ./autogen.sh >/dev/null
# shellcheck disable=SC2086
CONFIG_SITE="$depends_linux/share/config.site" build ./configure $CONFIGURE_ARGS >/dev/null
build make -j"$JOBS" >/dev/null

stage="$BUILD_ROOT/stage/sequentia-core-$version"
rm -rf "$BUILD_ROOT/stage"
mkdir -p "$stage/bin" "$stage/price-server"
for b in sequentiad sequentia-cli sequentia-tx sequentia-util sequentia-wallet; do
  [ -x "src/$b" ] || die "expected src/$b after the build"
  install -m755 "src/$b" "$stage/bin/$b"
done
if [ -x src/qt/sequentia-qt ]; then
  install -m755 src/qt/sequentia-qt "$stage/bin/sequentia-qt"
else
  log "WARNING: no GUI built (src/qt/sequentia-qt missing); shipping daemon + tools only"
fi
strip "$stage"/bin/* 2>/dev/null || true
for f in price_server.py README.md config.example.json ORACLE-AND-REFERENCE-DESIGN.md; do
  [ -f "contrib/price-server/$f" ] && cp "contrib/price-server/$f" "$stage/price-server/"
done

tar -C "$BUILD_ROOT/stage" -czf "$DOWNLOAD_DIR/$linux_art.part" "sequentia-core-$version"
mv "$DOWNLOAD_DIR/$linux_art.part" "$DOWNLOAD_DIR/$linux_art"
log "published $linux_art ($(du -h "$DOWNLOAD_DIR/$linux_art" | cut -f1))"

# --- Windows -----------------------------------------------------------------
# Cross build, so the tree must be reconfigured for the other host. Without the
# distclean a bare `make` silently reconfigures native and dies confusingly.
if [ "$BUILD_WINDOWS" = "1" ]; then
  depends_win="$REPO_DIR/depends/x86_64-w64-mingw32"
  if [ ! -d "$depends_win" ]; then
    log "WARNING: no depends prefix $depends_win; skipping the Windows installer"
  elif ! command -v makensis >/dev/null; then
    log "WARNING: makensis not installed; skipping the Windows installer"
  else
    log "building Windows installer"
    build make distclean >/dev/null 2>&1 || true
    build ./autogen.sh >/dev/null
    # shellcheck disable=SC2086
    CONFIG_SITE="$depends_win/share/config.site" build ./configure --prefix=/ $CONFIGURE_ARGS >/dev/null
    grep -q 'EXEEXT = \.exe' Makefile || die "configure produced a native build, not a cross build"
    # SHA256-pinned embeddable interpreter the installer bundles with the sidecar.
    build bash contrib/price-server/fetch-embeddable-python.sh >/dev/null
    build make -j"$JOBS" >/dev/null
    build make deploy >/dev/null
    # The tarname changed with the binary rename, so find the artifact rather
    # than assume its prefix.
    produced=$(ls -t ./*-win64-setup.exe 2>/dev/null | head -1) \
      || die "make deploy produced no installer"
    [ -n "$produced" ] || die "make deploy produced no installer"
    cp "$produced" "$DOWNLOAD_DIR/$win_art.part"
    mv "$DOWNLOAD_DIR/$win_art.part" "$DOWNLOAD_DIR/$win_art"
    log "published $win_art ($(du -h "$DOWNLOAD_DIR/$win_art" | cut -f1))"
  fi
fi

# --- Point the page at what actually exists -----------------------------------
# Each card is rewritten from the newest file PRESENT, not from the version we
# just built. If the Windows half was skipped or failed, its card keeps offering
# the previous installer instead of turning into a 404.
newest() { ls -1 "$DOWNLOAD_DIR"/$1 2>/dev/null | xargs -r -n1 basename | sort -V | tail -1; }
index="$DOWNLOAD_DIR/index.html"
if [ -f "$index" ]; then
  cp -p "$index" "$index.bak-publish"
  newest_linux=$(newest 'sequentia-core-*-linux-x86_64.tar.gz')
  newest_win=$(newest 'sequentia-core-*-win64-setup.exe')
  [ -n "$newest_linux" ] && sed -i -E "s#sequentia-core-[0-9][0-9.]*-linux-x86_64\.tar\.gz#$newest_linux#g" "$index"
  [ -n "$newest_win" ]   && sed -i -E "s#sequentia-core-[0-9][0-9.]*-win64-setup\.exe#$newest_win#g" "$index"
  # The single version label tracks the Linux build, which is the one this box
  # can always produce; each card still names its own file.
  [ -n "$newest_linux" ] && sed -i -E "s#(<span class=\"ver\">version )[0-9][0-9.]*#\1$(echo "$newest_linux" | sed -E 's#sequentia-core-([0-9.]+)-linux.*#\1#')#" "$index"
  log "index.html now offers: ${newest_linux:-none} / ${newest_win:-none}"
fi

echo "$latest_tag" > "$state_file"
log "done: $latest_tag published"
