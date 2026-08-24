# shellcheck shell=bash
# shellcheck disable=SC2034  # PRODUCT_* are read by the driver, not used here
# The Sequentia browser wallet extension.
#
# Versioned by manifest.json, which is the version the browser itself shows, so
# it is the only number a user can compare against what they have installed.
# There is no build step: the extension is the repository, minus its development
# files, zipped.
# Sorting and version comparison below are locale-sensitive.
export LC_ALL=C

PRODUCT_NAME="extension"
PRODUCT_REPO="${SEQ_EXTENSION_REPO:-https://github.com/ConcatenaLabs/sequentia-extension.git}"
# Both spellings, new one first. The extension was renamed to Ambra, and the old
# artifacts stay on the download server, so the page has to keep resolving a link
# to either while the transition settles. Drop the second pattern once no card
# names it.
PRODUCT_INDEX_GLOB="ambra-*-chromium.zip sequentia-wallet-extension-*.zip"

# Empty means "ask the remote what its default branch is".
EXTENSION_BRANCH="${SEQ_EXTENSION_BRANCH:-}"

requirements() {
  have zip || { echo "zip not installed (apt-get install zip)"; return 1; }
  have git || { echo "git missing"; return 1; }
  return 0
}

branch() { echo "${EXTENSION_BRANCH:-$(default_branch "$PRODUCT_REPO")}"; }

remote_version() {
  # Read manifest.json from the branch tip without a checkout, so the cheap
  # "has anything changed" path stays cheap.
  local dir="$BUILD_ROOT/src/extension" br; br="$(branch)"
  [ -n "$br" ] || return 1
  if [ ! -d "$dir/.git" ]; then
    git clone -q "$PRODUCT_REPO" "$dir" >/dev/null 2>&1 || return 1
  fi
  git -C "$dir" fetch -q origin "$br" || return 1
  git -C "$dir" show "origin/$br:manifest.json" 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("version",""))'
}

build() {
  local version="$1" out="$2" br; br="$(branch)"
  local dir; dir="$(prepare_checkout extension "$PRODUCT_REPO" "origin/$br")"
  cd "$dir" || return 1

  # What a user installs must not carry our development scaffolding: CLAUDE.md
  # is instructions to an agent, doc/ and test material are noise, and .git would
  # multiply the download size for nothing.
  # This script names the artifact, not the extension repository: the zip is made
  # here, out of the checkout. Mirrors the Android spelling, ambra-<version>-...
  local zipfile="$out/ambra-$version-chromium.zip"
  nice -n "$NICE" zip -q -r "$zipfile" . \
    -x '.git/*' '.github/*' 'doc/*' 'docs/*' 'test/*' 'tests/*' \
       'CLAUDE.md' '*.md.bak' '.gitignore' '*.zip'

  [ -s "$zipfile" ] || { log "[extension] zip produced nothing"; return 1; }
  # A manifest that does not parse means an extension that will not load.
  python3 -c 'import json;json.load(open("manifest.json"))' \
    || { log "[extension] manifest.json does not parse"; return 1; }
}
