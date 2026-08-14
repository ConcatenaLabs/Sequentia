# Sequentia Core: the node, the wallet and the GUI.
#
# Versioned by git TAG, unlike the other products here, because
# doc/sequentia/release-versioning.md requires it: a tag is the only reliable map
# from a version a node reports on the wire back to the code it is running.
PRODUCT_NAME="node"
PRODUCT_REPO="${SEQ_NODE_REPO:-https://github.com/GracedEternalKingCabbageMan/Sequentia.git}"
PRODUCT_INDEX_GLOB="sequentia-core-*-linux-x86_64.tar.gz sequentia-core-*-win64-setup.exe"

# --enable-any-asset-fees is not optional for a published build: without it fee
# amounts are labelled BTC/sat, which are Bitcoin's units and not this chain's.
NODE_CONFIGURE_ARGS="${SEQ_NODE_CONFIGURE_ARGS:---disable-tests --disable-bench --enable-any-asset-fees}"
# 0 publishes the Linux tarball only; the page then keeps the previous installer.
NODE_BUILD_WINDOWS="${SEQ_NODE_BUILD_WINDOWS:-1}"

requirements() {
  have git || { echo "git missing"; return 1; }
  have g++ || { echo "no C++ compiler"; return 1; }
  return 0
}

remote_version() {
  # sort -V, so v24.0.10 beats v24.0.9.
  git ls-remote --tags --refs "$PRODUCT_REPO" 'v[0-9]*' \
    | awk '{print $2}' | sed 's#refs/tags/##' | sort -V | tail -1
}

build() {
  local version="$1" out="$2"
  local v="${version#v}"
  local dir; dir="$(prepare_checkout node "$PRODUCT_REPO" "refs/tags/$version")"
  cd "$dir"

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
  if [ -x src/qt/sequentia-qt ]; then
    install -m755 src/qt/sequentia-qt "$stage/bin/sequentia-qt"
  else
    log "[node] WARNING: no GUI built; shipping daemon and tools only"
  fi
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
  local produced; produced="$(ls -t ./*-win64-setup.exe 2>/dev/null | head -1 || true)"
  [ -n "$produced" ] || { log "[node] make deploy produced no installer"; return 1; }
  cp "$produced" "$out/sequentia-core-$v-win64-setup.exe"
}
