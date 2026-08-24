# shellcheck shell=bash
# shellcheck disable=SC2034  # PRODUCT_* are read by the driver, not used here
# Sequentia Core: the node, the wallet and the GUI.
#
# Versioned by git TAG, unlike the other products here, because
# doc/sequentia/release-versioning.md requires it: a tag is the only reliable map
# from a version a node reports on the wire back to the code it is running.
# Sorting and version comparison below are locale-sensitive.
export LC_ALL=C

PRODUCT_NAME="node"
PRODUCT_REPO="${SEQ_NODE_REPO:-https://github.com/ConcatenaLabs/Sequentia.git}"
PRODUCT_INDEX_GLOB="sequentia-core-*-linux-x86_64.tar.gz sequentia-core-*-win64-setup.exe"

# --enable-any-asset-fees is not optional for a published build: without it fee
# amounts are labelled BTC/sat, which are Bitcoin's units and not this chain's.
# --with-gui=qt5 is explicit rather than left to autodetection: the download page
# offers this as "Desktop wallet + node", so a build that quietly finds no Qt and
# produces a headless tarball has built the wrong product. Explicit means configure
# fails loudly instead.
NODE_CONFIGURE_ARGS="${SEQ_NODE_CONFIGURE_ARGS:---with-gui=qt5 --disable-tests --disable-bench --enable-any-asset-fees}"
# 0 publishes the Linux tarball only; the page then keeps the previous installer.
NODE_BUILD_WINDOWS="${SEQ_NODE_BUILD_WINDOWS:-1}"

requirements() {
  have git || { echo "git missing"; return 1; }
  have g++ || { echo "no C++ compiler"; return 1; }
  return 0
}

# Say so, loudly, when master carries a version that was never tagged.
#
# This product releases on a TAG, so bumping _CLIENT_VERSION in configure.ac and
# merging it does nothing here: the newest tag is unchanged, the run logs the
# reassuring "[node] up to date at v<older>", and the download page keeps serving
# the old build indefinitely. That is exactly how 24.2.0 sat unpublished -- the
# bump merged to master, no v24.2.0 tag was ever pushed, and nothing anywhere
# reported a problem, because from the publisher's point of view there was none.
#
# So an untagged bump has to announce itself. This warns rather than fails: the
# release is still a deliberate act, and a red service would bury the real build
# failures the exit status is there to report.
#
# The version to compare against is read from the publisher's own clone, which
# the unit pins to this repo at origin/master -- this recipe is a file inside it.
warn_if_master_is_untagged() {
  local newest_tag="$1"
  local root
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)" || return 0
  local ac="$root/configure.ac"
  [ -f "$ac" ] || return 0

  local major minor build
  major="$(sed -n 's/^define(_CLIENT_VERSION_MAJOR, *\([0-9]*\)).*/\1/p' "$ac")"
  minor="$(sed -n 's/^define(_CLIENT_VERSION_MINOR, *\([0-9]*\)).*/\1/p' "$ac")"
  build="$(sed -n 's/^define(_CLIENT_VERSION_BUILD, *\([0-9]*\)).*/\1/p' "$ac")"
  [ -n "$major" ] && [ -n "$minor" ] && [ -n "$build" ] || return 0

  local master_version="v$major.$minor.$build"
  [ "$master_version" != "$newest_tag" ] || return 0
  # Only when master is AHEAD. A tag above master is a release branch, not this.
  [ "$(printf '%s\n%s\n' "$newest_tag" "$master_version" | sort -V | tail -1)" \
      = "$master_version" ] || return 0

  log "[node] WARNING: master is at $master_version but the newest tag is ${newest_tag:-none}."
  log "[node] WARNING: the download page stays on ${newest_tag:-nothing} until that version is tagged:"
  log "[node] WARNING:   git tag -a $master_version <commit> -m 'Sequentia Core ${master_version#v}'"
  log "[node] WARNING:   git push origin $master_version"
}

remote_version() {
  # sort -V, so v24.0.10 beats v24.0.9.
  local newest
  newest="$(git ls-remote --tags --refs "$PRODUCT_REPO" 'v[0-9]*' \
    | awk '{print $2}' | sed 's#refs/tags/##' | sort -V | tail -1)"
  # To stderr, so the driver's "$(remote_version)" still captures only the version.
  warn_if_master_is_untagged "$newest" >&2
  printf '%s\n' "$newest"
}

build() {
  local version="$1" out="$2"
  local v="${version#v}"
  local dir; dir="$(prepare_checkout node "$PRODUCT_REPO" "refs/tags/$version")"
  cd "$dir" || return 1

  local depends_linux="$dir/depends/x86_64-pc-linux-gnu"
  if [ ! -d "$depends_linux" ]; then
    log "[node] missing depends prefix $depends_linux"
    log "[node] build it once: nice -n 19 make -C depends HOST=x86_64-pc-linux-gnu -j$JOBS"
    return 1
  fi

  # Keep the output. Discarding it meant a failed unattended build reported only
  # "BUILD FAILED" and the reason had to be reproduced by hand afterwards.
  local blog="$BUILD_ROOT/node-linux.log"
  node_step() {
    local what="$1"; shift
    if ! nice -n "$NICE" "$@" >>"$blog" 2>&1; then
      log "[node] $what FAILED; last lines:"; tail -15 "$blog" | sed 's/^/    /'; return 1
    fi
  }

  log "[node] building Linux ($JOBS jobs, nice $NICE) -> $blog"
  : > "$blog"
  # Start from a clean tree. The same checkout builds both hosts, so it may still
  # be configured for the mingw cross build from a previous run -- and configuring
  # native on top of that leaves libtool .la files still naming Windows libraries,
  # so the native link fails with "cannot find -lkernel32" and friends, which reads
  # like a missing dependency rather than a stale build.
  nice -n "$NICE" make distclean >>"$blog" 2>&1 || true
  node_step autogen ./autogen.sh || return 1
  # shellcheck disable=SC2086
  CONFIG_SITE="$depends_linux/share/config.site" node_step configure ./configure $NODE_CONFIGURE_ARGS || return 1
  node_step make make -j"$JOBS" || return 1

  local stage="$BUILD_ROOT/stage/sequentia-core-$v"
  rm -rf "$BUILD_ROOT/stage"; mkdir -p "$stage/bin" "$stage/price-server"
  local b
  for b in sequentiad sequentia-cli sequentia-tx sequentia-util sequentia-wallet; do
    [ -x "src/$b" ] || { log "[node] expected src/$b after the build"; return 1; }
    install -m755 "src/$b" "$stage/bin/$b"
  done
  # Refuse to publish a "desktop wallet" with no desktop wallet in it. This warned
  # and shipped once, when moving the build clone left the depends prefixes
  # pointing at their old absolute path so Qt became undetectable -- a broken
  # environment that produced a plausible-looking tarball, which is the worst kind.
  if [ ! -x src/qt/sequentia-qt ]; then
    log "[node] no GUI at src/qt/sequentia-qt -- the page offers this as a desktop wallet, refusing to publish without it"
    log "[node] (check that depends/x86_64-pc-linux-gnu still holds Qt and that its paths match this checkout)"
    return 1
  fi
  install -m755 src/qt/sequentia-qt "$stage/bin/sequentia-qt"
  strip "$stage"/bin/* 2>/dev/null || true
  local f
  for f in price_server.py README.md config.example.json ORACLE-AND-REFERENCE-DESIGN.md; do
    [ -f "contrib/price-server/$f" ] && cp "contrib/price-server/$f" "$stage/price-server/"
  done
  tar -C "$BUILD_ROOT/stage" -czf "$out/sequentia-core-$v-linux-x86_64.tar.gz" "sequentia-core-$v"

  if [ "$NODE_BUILD_WINDOWS" != "1" ]; then
    log "[node] Windows build disabled"
    return 0
  fi
  local depends_win="$dir/depends/x86_64-w64-mingw32"
  if [ ! -d "$depends_win" ]; then
    log "[node] no Windows depends prefix; skipping the installer (page keeps the previous one)"
    return 0
  fi
  if ! have makensis; then
    log "[node] makensis not installed; skipping the installer"
    return 0
  fi

  local wlog="$BUILD_ROOT/node-windows.log"
  win_step() {
    local what="$1"; shift
    if ! nice -n "$NICE" "$@" >>"$wlog" 2>&1; then
      log "[node] windows $what FAILED; last lines:"; tail -15 "$wlog" | sed 's/^/    /'; return 1
    fi
  }

  log "[node] building the Windows installer -> $wlog"
  : > "$wlog"
  # Cross build: the tree must be reconfigured for the other host. Without the
  # distclean a bare make silently reconfigures native and dies confusingly.
  nice -n "$NICE" make distclean >>"$wlog" 2>&1 || true
  win_step autogen ./autogen.sh || return 1
  # shellcheck disable=SC2086
  CONFIG_SITE="$depends_win/share/config.site" win_step configure ./configure --prefix=/ $NODE_CONFIGURE_ARGS || return 1
  grep -q 'EXEEXT = \.exe' Makefile || { log "[node] configure produced a native build, not a cross build"; return 1; }
  # SHA256-pinned interpreter the installer bundles with the price-server sidecar.
  win_step fetch-python bash contrib/price-server/fetch-embeddable-python.sh || return 1
  win_step make make -j"$JOBS" || return 1
  win_step deploy make deploy || return 1

  # The package tarname changed with the binary rename, so find the artifact
  # rather than assume its prefix.
  local produced
  produced="$(find . -maxdepth 1 -type f -name '*-win64-setup.exe' -printf '%T@ %p\n' 2>/dev/null \
              | sort -rn | head -1 | cut -d' ' -f2-)"
  [ -n "$produced" ] || { log "[node] make deploy produced no installer"; return 1; }
  cp "$produced" "$out/sequentia-core-$v-win64-setup.exe"
}
