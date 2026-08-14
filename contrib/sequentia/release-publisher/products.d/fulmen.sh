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

  # Separate invocations, not one with three targets. electron-builder fails the
  # whole command if any target fails, so a wine problem in the NSIS step threw
  # away the AppImage and the zip that had already built successfully -- the page
  # then kept all three at the previous version because of one broken installer.
  # Run them independently so each publishes on its own merit, and put NSIS last:
  # it is the only one that needs wine.
  local built=0 tgt rc
  for tgt in "--linux AppImage" "--win zip" "--win nsis"; do
    log "[fulmen] building $tgt"
    # shellcheck disable=SC2086
    if nice -n "$NICE" npx electron-builder $tgt > "$BUILD_ROOT/fulmen-$(echo "$tgt" | tr -d ' -').log" 2>&1; then
      built=1
    else
      rc=$?
      log "[fulmen] $tgt FAILED (rc=$rc); last lines:"
      tail -12 "$BUILD_ROOT/fulmen-$(echo "$tgt" | tr -d ' -').log" | sed 's/^/    /'
    fi
  done
  [ "$built" = "1" ] || { log "[fulmen] every target failed"; return 1; }

  # electron-builder names artifacts from package.json; collect whatever it
  # produced rather than predicting the exact spelling.
  shopt -s nullglob
  local found=0 f
  for f in dist/*.AppImage; do cp "$f" "$out/Fulmen-$version-linux-x86_64.AppImage"; found=1; done
  for f in dist/*-win.zip dist/*win*.zip; do cp "$f" "$out/Fulmen-$version-win64.zip"; found=1; break; done
  # electron-builder names the NSIS output from package.json, so match on the
  # extension rather than predicting the spelling, and exclude the zip.
  for f in dist/*.exe; do cp "$f" "$out/Fulmen-Setup-$version.exe"; found=1; break; done
  shopt -u nullglob
  [ "$found" = "1" ] || { log "[fulmen] electron-builder produced no recognisable artifacts"; return 1; }

  # A Windows installer built under wine that nobody has run on Windows is a
  # coin toss, so refuse to publish one that is not even the right kind of file:
  # a PE executable. This catches the common wine failure where the NSIS step
  # half-succeeds and leaves a stub behind.
  if [ -f "$out/Fulmen-Setup-$version.exe" ]; then
    if ! file "$out/Fulmen-Setup-$version.exe" | grep -qi 'PE32\|MS Windows'; then
      log "[fulmen] the produced Setup.exe is not a Windows executable; dropping it"
      rm -f "$out/Fulmen-Setup-$version.exe"
    fi
  else
    log "[fulmen] no installer produced; page keeps the previous one"
  fi
}
