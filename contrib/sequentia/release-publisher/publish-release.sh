#!/bin/bash
# Publish new releases to the download page, unattended, for every product the
# page offers -- not just the node.
#
# This is a driver. It knows nothing about how any product is built: each one is
# a recipe in products.d/, and adding a product to the download page means adding
# one file there and nothing else.
#
# For every recipe it asks "what version is published upstream", compares that
# with what it published last, and if they differ it builds that version and
# copies the artifacts into the download directory. Then it points each card on
# the page at the newest file that actually exists.
#
# A product whose toolchain is missing is SKIPPED with a reason, and the rest
# still publish. That matters: the box can build the node and the extension
# today, while Ambra needs an Android toolchain and a signing key that are a
# separate decision. One product's prerequisites must never block the others.
#
# The build is niced and job-limited throughout, because this machine also
# produces blocks and starving it is how the testnet stalls.
set -euo pipefail

BUILD_ROOT="${SEQ_BUILD_ROOT:-/root/sequentia/release-build}"
STATE_DIR="${SEQ_STATE_DIR:-$BUILD_ROOT/state}"
DOWNLOAD_DIR="${SEQ_DOWNLOAD_DIR:-/root/sequentia/downloads}"
RECIPE_DIR="${SEQ_RECIPE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/products.d}"
JOBS="${SEQ_BUILD_JOBS:-4}"
NICE="${SEQ_BUILD_NICE:-19}"
# Space-separated recipe names to run; empty means all of them.
ONLY="${SEQ_ONLY:-}"

export BUILD_ROOT DOWNLOAD_DIR JOBS NICE

log()  { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die()  { log "ERROR: $*"; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
export -f log have

mkdir -p "$STATE_DIR" "$BUILD_ROOT" "$DOWNLOAD_DIR"

# One publisher at a time: a timer tick during a two-hour build must not start a
# second build in the same trees.
exec 9>"$STATE_DIR/publish.lock"
if ! flock -n 9; then
  log "another publish is running; nothing to do"
  exit 0
fi

# --- Keep a product's checkout at the version we are about to build -----------
# Recipes call this; it is the only git any of them need.
prepare_checkout() {
  local name="$1" url="$2" ref="$3" dir="$BUILD_ROOT/src/$name"
  mkdir -p "$BUILD_ROOT/src"
  if [ ! -d "$dir/.git" ]; then
    git clone -q "$url" "$dir"
  fi
  git -C "$dir" fetch -q --tags --force origin
  git -C "$dir" reset -q --hard
  # depends/ prefixes cost hours to rebuild; never let clean take them.
  git -C "$dir" clean -qfd -e depends
  git -C "$dir" checkout -q "$ref"
  echo "$dir"
}
export -f prepare_checkout

# Read a JSON string field without needing jq.
json_field() {
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2],""))' "$1" "$2"
}
export -f json_field

# Ask the remote which branch is its default, rather than assuming. These repos
# do not agree -- two are on master and one on main -- and a wrong guess fails as
# "couldn't find remote ref", which reads like a network problem rather than a
# wrong name.
default_branch() {
  git ls-remote --symref "$1" HEAD 2>/dev/null \
    | awk '/^ref:/ { sub("refs/heads/", "", $2); print $2; exit }'
}
export -f default_branch

# --- Run one recipe ------------------------------------------------------------
# Each recipe defines: PRODUCT_NAME, PRODUCT_REPO, PRODUCT_INDEX_GLOB,
# remote_version(), and build(version, outdir). It may define requirements(),
# which prints a reason to skip and returns non-zero when it cannot build.
run_recipe() {
  local recipe="$1"
  local name; name="$(basename "$recipe" .sh)"

  ( # subshell: a recipe's variables and cd must not leak into the next one
    set -euo pipefail
    # shellcheck disable=SC1090
    source "$recipe"

    if declare -F requirements >/dev/null; then
      local why
      if ! why="$(requirements)"; then
        log "[$name] skipped: ${why:-prerequisites not met}"
        return 0
      fi
    fi

    local version
    version="$(remote_version)" || { log "[$name] could not read upstream version; skipping"; return 0; }
    [ -n "$version" ] || { log "[$name] upstream version is empty; skipping"; return 0; }

    local state_file="$STATE_DIR/$name.version"
    local published; published="$(cat "$state_file" 2>/dev/null || echo "")"
    if [ "$version" = "$published" ]; then
      log "[$name] up to date at $version"
      return 0
    fi

    # Never move a card backwards. The state file only knows what THIS script
    # published; artifacts put there by hand over the years are older history it
    # has never seen, and some of them are ahead of what their repository's
    # version field currently says. Publishing then would quietly offer users a
    # downgrade, which for a wallet is worse than offering nothing.
    local newest_present offered
    newest_present="$(cd "$DOWNLOAD_DIR" && ls -1 ${PRODUCT_INDEX_GLOB%% *} 2>/dev/null | sort -V | tail -1 || true)"
    if [ -n "$newest_present" ]; then
      offered="$(printf '%s' "$newest_present" | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)"
      if [ -n "$offered" ] && [ "$(printf '%s\n%s\n' "$offered" "${version#v}" | sort -V | tail -1)" != "${version#v}" ]; then
        log "[$name] upstream says $version but the page already offers $offered; refusing to downgrade"
        return 0
      fi
    fi

    log "[$name] new version $version (last published: ${published:-none})"
    local out="$BUILD_ROOT/out/$name"
    rm -rf "$out"; mkdir -p "$out"

    if ! build "$version" "$out"; then
      log "[$name] BUILD FAILED at $version; the page is unchanged and the next run will retry"
      return 1
    fi

    local published_any=0
    shopt -s nullglob
    for f in "$out"/*; do
      [ -f "$f" ] || continue
      local base; base="$(basename "$f")"
      # Land it complete or not at all: a half-copied artifact must never be
      # servable, and the page is only repointed after this.
      cp "$f" "$DOWNLOAD_DIR/$base.part"
      mv "$DOWNLOAD_DIR/$base.part" "$DOWNLOAD_DIR/$base"
      log "[$name] published $base ($(du -h "$DOWNLOAD_DIR/$base" | cut -f1))"
      published_any=1
    done
    shopt -u nullglob

    [ "$published_any" = "1" ] || { log "[$name] build produced no artifacts; not recording $version"; return 1; }
    echo "$version" > "$state_file"
    log "[$name] done at $version"
  )
}

# --- Point every card at a file that exists -----------------------------------
# Rewritten from what is PRESENT, never from what this run happened to build, so
# a product that was skipped or failed keeps offering its previous version
# instead of turning into a 404.
update_index() {
  local index="$DOWNLOAD_DIR/index.html"
  [ -f "$index" ] || { log "no index.html; nothing to repoint"; return 0; }
  cp -p "$index" "$index.bak-publish"

  local recipe name glob newest
  for recipe in "$RECIPE_DIR"/*.sh; do
    [ -f "$recipe" ] || continue
    name="$(basename "$recipe" .sh)"
    # Read the glob without running the recipe's build.
    glob="$(grep -m1 '^PRODUCT_INDEX_GLOB=' "$recipe" | cut -d= -f2- | tr -d '"')"
    [ -n "$glob" ] || continue
    # Split on spaces WITHOUT letting bash expand the patterns. `for one in $glob`
    # looks equivalent and is not: unquoted expansion also does pathname
    # expansion, so whenever the working directory happened to contain a matching
    # file, the pattern silently became that filename and every substitution
    # turned into a no-op. It survived because systemd runs this from /, where
    # nothing matches -- so it worked in production and failed under test.
    local -a globs=()
    read -r -a globs <<< "$glob"
    for one in "${globs[@]}"; do
      # `|| true` is load-bearing: ls fails when a pattern matches nothing, and
      # under set -e with pipefail that killed the whole driver before a single
      # card was rewritten. A product with no artifacts yet is normal -- a newly
      # added recipe has none until its first build -- so it must be skipped, not
      # fatal.
      # Only files whose name carries a version, and only real files. The download
      # directory also holds unversioned aliases -- ambra-latest.apk and friends,
      # symlinks kept for stable URLs -- and "latest" sorts above every number, so
      # they won every comparison and the card ended up naming a file with no
      # version in it. Since each card shows its filename as the version, that
      # silently removed the version from the page.
      newest="$(find "$DOWNLOAD_DIR" -maxdepth 1 -type f -name "$one" -printf '%f\n' 2>/dev/null \
                 | grep -E '[0-9]+\.[0-9]+' | sort -V | tail -1 || true)"
      [ -n "$newest" ] || continue
      # Turn the concrete filename back into a pattern by replacing its version.
      local pat; pat="$(printf '%s' "$one" | sed 's/\*/[0-9][0-9.]*/g')"
      sed -i -E "s#$pat#$newest#g" "$index"
    done
  done

  # The single version label tracks the node, which is what the page is about.
  local newest_node
  newest_node="$( (cd "$DOWNLOAD_DIR" && ls -1 sequentia-core-*-linux-x86_64.tar.gz 2>/dev/null || true) | sort -V | tail -1 )"
  if [ -n "$newest_node" ]; then
    local v; v="$(printf '%s' "$newest_node" | sed -E 's#sequentia-core-([0-9.]+)-linux.*#\1#')"
    sed -i -E "s#(<span class=\"ver\">version )[0-9][0-9.]*#\1$v#" "$index"
  fi

  # Only what the page actually links: the surrounding prose mentions filenames
  # too ("run Fulmen.exe"), and reporting those as offered artifacts is noise.
  log "page now offers:"
  grep -oE 'href="[^"]+\.(tar\.gz|exe|zip|AppImage|apk)"' "$index" \
    | sed -E 's#href="(.*)"#\1#' | sort -u | sed 's/^/  /'
}

# --- Go ------------------------------------------------------------------------
[ -d "$RECIPE_DIR" ] || die "no recipes at $RECIPE_DIR"

failed=0
for recipe in "$RECIPE_DIR"/*.sh; do
  [ -f "$recipe" ] || continue
  name="$(basename "$recipe" .sh)"
  if [ -n "$ONLY" ] && ! printf '%s ' $ONLY | grep -q "$name "; then
    continue
  fi
  # One product failing must not stop the others.
  run_recipe "$recipe" || failed=1
done

update_index

[ "$failed" = "0" ] || die "at least one product failed to publish (see above)"
log "all products up to date"
