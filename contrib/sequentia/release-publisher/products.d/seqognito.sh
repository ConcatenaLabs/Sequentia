# shellcheck shell=bash
# shellcheck disable=SC2034  # PRODUCT_* are read by the driver, not used here
# Seqognito: the Electron mixing wallet (CoinJoin over confidential transactions,
# everything over Tor).
#
# Versioned by package.json, like Fulmen, so the version and the artifact names
# cannot drift apart.
#
# The one thing this recipe does that no other does: it builds the wasm signer
# first. Seqognito does not vendor `ui/pkg` -- that is a 12 MB build artefact
# whose source of truth is SWK -- so the app cannot be packaged without compiling
# lwk_wasm from that repository at the version it is on. A published installer
# therefore carries a signer built from source in this run, not one that happened
# to be lying in the working directory.
# Sorting and version comparison below are locale-sensitive.
export LC_ALL=C

# rustup installs into the user's home, which a systemd unit's PATH does not
# include. Without this the recipe is skipped as "cargo not installed" on a box
# that has cargo.
[ -d "$HOME/.cargo/bin" ] && export PATH="$HOME/.cargo/bin:$PATH"

PRODUCT_NAME="seqognito"
PRODUCT_REPO="${SEQ_SEQOGNITO_REPO:-https://github.com/ConcatenaLabs/seqognito.git}"
PRODUCT_INDEX_GLOB="Seqognito-*-linux-x86_64.AppImage Seqognito-*-win64.zip Seqognito-Setup-*.exe"

SEQOGNITO_SWK_REPO="${SEQ_SWK_REPO:-https://github.com/ConcatenaLabs/SWK.git}"
SEQOGNITO_SWK_BRANCH="${SEQ_SWK_BRANCH:-sequentia}"

# Empty means "ask the remote what its default branch is".
SEQOGNITO_BRANCH="${SEQ_SEQOGNITO_BRANCH:-}"
branch() { echo "${SEQOGNITO_BRANCH:-$(default_branch "$PRODUCT_REPO")}"; }

requirements() {
  have npm || { echo "npm not installed"; return 1; }
  have node || { echo "node not installed"; return 1; }
  # The wasm signer is compiled here, so its toolchain is a hard requirement
  # rather than something to discover halfway through a build.
  have cargo || { echo "cargo not installed (the wasm signer is built from source)"; return 1; }
  have wasm-pack || { echo "wasm-pack not installed (the wasm signer is built from source)"; return 1; }
  return 0
}

remote_version() {
  local dir="$BUILD_ROOT/src/seqognito" br; br="$(branch)"
  [ -n "$br" ] || return 1
  if [ ! -d "$dir/.git" ]; then
    git clone -q "$PRODUCT_REPO" "$dir" >/dev/null 2>&1 || return 1
  fi
  git -C "$dir" fetch -q origin "$br" || return 1
  git -C "$dir" show "origin/$br:package.json" 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("version",""))'
}

build() {
  local version="$1" out="$2" br; br="$(branch)"
  local dir; dir="$(prepare_checkout seqognito "$PRODUCT_REPO" "origin/$br")"

  # --- the wasm signer, from source -------------------------------------------
  # prepare_checkout cleans untracked files, so this is a cold Rust build every
  # time -- which is the honest cost of not shipping a prebuilt binary, and it
  # only happens when Seqognito's version actually changed.
  local swk
  swk="$(prepare_checkout swk "$SEQOGNITO_SWK_REPO" "origin/$SEQOGNITO_SWK_BRANCH")" || {
    log "[seqognito] could not check out SWK"; return 1; }
  log "[seqognito] building the wasm signer from SWK (cold build; this takes a while)"
  if ! ( cd "$swk/lwk_wasm" && nice -n "$NICE" wasm-pack build --target web --release ) \
       > "$BUILD_ROOT/seqognito-wasm.log" 2>&1; then
    log "[seqognito] wasm build FAILED; last lines:"
    tail -12 "$BUILD_ROOT/seqognito-wasm.log" | sed 's/^/    /'
    return 1
  fi
  # The app imports coinjoinSignInputs by namespace and degrades without it, so a
  # signer missing the mixing exports would package happily and ship a wallet that
  # cannot mix. Check for them here, where it is still a build failure.
  grep -q 'coinjoinSignInputs' "$swk/lwk_wasm/pkg/lwk_wasm.js" || {
    log "[seqognito] the built signer has no coinjoin exports; refusing to package a wallet that cannot mix"
    return 1; }
  rm -rf "$dir/ui/pkg"
  cp -r "$swk/lwk_wasm/pkg" "$dir/ui/pkg"

  cd "$dir" || return 1
  log "[seqognito] installing dependencies"
  # ci, not install: build from the lockfile so a published artifact is
  # reproducible from the commit rather than from whatever npm resolved today.
  nice -n "$NICE" npm ci --no-audit --no-fund >/dev/null 2>&1 \
    || nice -n "$NICE" npm install --no-audit --no-fund >/dev/null 2>&1 \
    || { log "[seqognito] dependency install failed"; return 1; }

  # Separate invocations for the same reason as Fulmen: electron-builder fails the
  # whole command if any target fails, and NSIS is the only one that needs wine,
  # so a wine problem must not throw away the Linux and portable builds that
  # already succeeded. NSIS last, for the same reason.
  local ok_appimage=0 ok_zip=0 ok_nsis=0 tgt rc slug
  for tgt in "--linux AppImage" "--win zip" "--win nsis"; do
    slug="$(echo "$tgt" | tr -d ' -')"
    log "[seqognito] building $tgt"
    # shellcheck disable=SC2086
    if nice -n "$NICE" npx electron-builder $tgt > "$BUILD_ROOT/seqognito-$slug.log" 2>&1; then
      case "$tgt" in
        *AppImage) ok_appimage=1 ;;
        *zip)      ok_zip=1 ;;
        *nsis)     ok_nsis=1 ;;
      esac
    else
      rc=$?
      log "[seqognito] $tgt FAILED (rc=$rc); last lines:"
      tail -12 "$BUILD_ROOT/seqognito-$slug.log" | sed 's/^/    /'
    fi
  done
  [ "$((ok_appimage + ok_zip + ok_nsis))" -gt 0 ] || { log "[seqognito] every target failed"; return 1; }

  # An Electron artifact carries a whole browser runtime plus a 12 MB signer, so
  # any of these is tens of megabytes. A few hundred KB means the build stopped
  # before the payload went in, which is exactly what a half-failed NSIS step
  # leaves behind -- a valid PE and a valid Nullsoft archive that installs nothing.
  # Size is the check that catches it; the file type does not.
  local MIN_BYTES="${SEQ_SEQOGNITO_MIN_ARTIFACT_BYTES:-20000000}"
  take() {  # take <built-ok> <source glob> <published name>
    local ok="$1" glob="$2" name="$3" f sz
    [ "$ok" = "1" ] || { log "[seqognito] $name: target did not succeed, leaving the published one alone"; return 0; }
    shopt -s nullglob
    for f in $glob; do
      sz=$(stat -c %s "$f")
      if [ "$sz" -lt "$MIN_BYTES" ]; then
        log "[seqognito] $name: $(basename "$f") is only $sz bytes, too small to contain the app; refusing it"
        shopt -u nullglob; return 0
      fi
      cp "$f" "$out/$name"
      shopt -u nullglob; return 0
    done
    shopt -u nullglob
    log "[seqognito] $name: the target succeeded but produced no file"
  }

  take "$ok_appimage" 'dist/*.AppImage'  "Seqognito-$version-linux-x86_64.AppImage"
  take "$ok_zip"      'dist/*win*.zip'   "Seqognito-$version-win64.zip"
  # electron-builder names the installer from package.json, so match the extension
  # rather than predicting the spelling.
  take "$ok_nsis"     'dist/*.exe'       "Seqognito-Setup-$version.exe"

  [ -n "$(ls -A "$out" 2>/dev/null)" ] || { log "[seqognito] nothing passed the checks"; return 1; }
}
