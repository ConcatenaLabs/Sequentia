# shellcheck shell=bash
# Fulmen: the Electron Lightning wallet.
#
# Versioned by package.json, which is what electron-builder stamps into the
# artifact names, so the version and the filenames cannot drift apart.
# Sorting and version comparison below are locale-sensitive.
export LC_ALL=C

# shellcheck disable=SC2034  # read by the driver, not used in this file
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
  cd "$dir" || return 1

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
  # A target that fails still leaves whatever it got as far as writing. electron-
  # builder's NSIS step assembles the installer and only then appends the app, so a
  # wine failure at the end leaves a VALID 170KB installer that installs nothing.
  # It is a real PE file and a real Nullsoft archive, so no format check will catch
  # it. Only publish what a SUCCEEDING target produced.
  local ok_appimage=0 ok_zip=0 ok_nsis=0 tgt rc slug
  for tgt in "--linux AppImage" "--win zip" "--win nsis"; do
    slug="$(echo "$tgt" | tr -d ' -')"
    log "[fulmen] building $tgt"
    # shellcheck disable=SC2086
    if nice -n "$NICE" npx electron-builder $tgt > "$BUILD_ROOT/fulmen-$slug.log" 2>&1; then
      case "$tgt" in
        *AppImage) ok_appimage=1 ;;
        *zip)      ok_zip=1 ;;
        *nsis)     ok_nsis=1 ;;
      esac
    else
      rc=$?
      log "[fulmen] $tgt FAILED (rc=$rc); last lines:"
      tail -12 "$BUILD_ROOT/fulmen-$slug.log" | sed 's/^/    /'
    fi
  done
  [ "$((ok_appimage + ok_zip + ok_nsis))" -gt 0 ] || { log "[fulmen] every target failed"; return 1; }

  # electron-builder names artifacts from package.json; collect whatever it
  # produced rather than predicting the exact spelling.
  # An Electron artifact carries a whole browser runtime, so any of these is tens
  # of megabytes. A few hundred KB means the build stopped before the payload went
  # in -- which is exactly what a half-failed NSIS step leaves behind. Size is the
  # check that catches it; the file type does not, because the stub is a perfectly
  # well-formed PE and Nullsoft archive.
  local MIN_BYTES="${SEQ_FULMEN_MIN_ARTIFACT_BYTES:-20000000}"
  take() {  # take <built-ok> <source glob> <published name>
    local ok="$1" glob="$2" name="$3" f sz
    [ "$ok" = "1" ] || { log "[fulmen] $name: target did not succeed, leaving the published one alone"; return 0; }
    shopt -s nullglob
    for f in $glob; do
      sz=$(stat -c %s "$f")
      if [ "$sz" -lt "$MIN_BYTES" ]; then
        log "[fulmen] $name: $(basename "$f") is only $sz bytes, too small to contain the app; refusing it"
        shopt -u nullglob; return 0
      fi
      cp "$f" "$out/$name"
      shopt -u nullglob; return 0
    done
    shopt -u nullglob
    log "[fulmen] $name: the target succeeded but produced no file"
  }

  take "$ok_appimage" 'dist/*.AppImage'            "Fulmen-$version-linux-x86_64.AppImage"
  take "$ok_zip"      'dist/*win*.zip'             "Fulmen-$version-win64.zip"
  # electron-builder names the installer from package.json, so match the extension
  # rather than predicting the spelling.
  take "$ok_nsis"     'dist/*.exe'                 "Fulmen-Setup-$version.exe"

  [ -n "$(ls -A "$out" 2>/dev/null)" ] || { log "[fulmen] nothing passed the checks"; return 1; }
}
