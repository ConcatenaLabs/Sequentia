# Fulmen: the Electron Lightning wallet.
#
# Versioned by package.json, which is what electron-builder stamps into the
# artifact names, so the version and the filenames cannot drift apart.
PRODUCT_NAME="fulmen"
PRODUCT_REPO="${SEQ_FULMEN_REPO:-https://github.com/GracedEternalKingCabbageMan/fulmen.git}"
PRODUCT_INDEX_GLOB="Fulmen-*-linux-x86_64.AppImage Fulmen-*-win64.zip Fulmen-Setup-*.exe"

# Empty means "ask the remote what its default branch is".
FULMEN_BRANCH="${SEQ_FULMEN_BRANCH:-}"
branch() { echo "${FULMEN_BRANCH:-$(default_branch "$PRODUCT_REPO")}"; }

requirements() {
  have npm || { echo "npm not installed"; return 1; }
  have node || { echo "node not installed"; return 1; }
  return 0
}

remote_version() {
  local dir="$BUILD_ROOT/src/fulmen" br; br="$(branch)"
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
  local dir; dir="$(prepare_checkout fulmen "$PRODUCT_REPO" "origin/$br")"
  cd "$dir"

  log "[fulmen] installing dependencies"
  # ci, not install: build from the lockfile so a published artifact is
  # reproducible from the commit rather than from whatever npm resolved today.
  nice -n "$NICE" npm ci --no-audit --no-fund >/dev/null 2>&1 \
    || nice -n "$NICE" npm install --no-audit --no-fund >/dev/null 2>&1 \
    || { log "[fulmen] dependency install failed"; return 1; }

  # AppImage is a native Linux target and needs nothing extra. The Windows ZIP
  # target does not need wine either, because it is a directory that gets
  # zipped -- unlike the NSIS installer, see below.
  log "[fulmen] building Linux AppImage and Windows zip"
  nice -n "$NICE" npx electron-builder --linux AppImage --win zip >/dev/null 2>&1 \
    || { log "[fulmen] electron-builder failed"; return 1; }

  # electron-builder names artifacts from package.json; collect whatever it
  # produced rather than predicting the exact spelling.
  shopt -s nullglob
  local found=0 f
  for f in dist/*.AppImage; do cp "$f" "$out/Fulmen-$version-linux-x86_64.AppImage"; found=1; done
  for f in dist/*-win.zip dist/*win*.zip; do cp "$f" "$out/Fulmen-$version-win64.zip"; found=1; break; done
  shopt -u nullglob
  [ "$found" = "1" ] || { log "[fulmen] electron-builder produced no recognisable artifacts"; return 1; }

  # The NSIS installer (Fulmen-Setup-<v>.exe) is deliberately NOT built here.
  # electron-builder needs wine to produce it on Linux, and a half-working wine
  # produces an installer that fails on a user's machine rather than failing in
  # the build. Until wine is installed and the result has been run on Windows,
  # the page keeps offering the previous Setup.exe, which is honest: that file
  # exists and works. The zip covers Windows users in the meantime.
  log "[fulmen] Setup.exe not built (needs wine); page keeps the previous installer"
}
