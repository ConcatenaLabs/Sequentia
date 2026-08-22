#!/usr/bin/env bash
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

# Which artifact a card ends up naming is decided by sorting and version
# comparison, and both are locale-sensitive.
export LC_ALL=C

set -euo pipefail

BUILD_ROOT="${SEQ_BUILD_ROOT:-/root/sequentia/release-build}"
STATE_DIR="${SEQ_STATE_DIR:-$BUILD_ROOT/state}"
DOWNLOAD_DIR="${SEQ_DOWNLOAD_DIR:-/root/sequentia/downloads}"
SIGNING_GNUPGHOME="${SEQ_SIGNING_GNUPGHOME:-/etc/sequentia/release-signing}"
RECIPE_DIR="${SEQ_RECIPE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/products.d}"
JOBS="${SEQ_BUILD_JOBS:-4}"
NICE="${SEQ_BUILD_NICE:-19}"
# Space-separated recipe names to run; empty means all of them.
ONLY="${SEQ_ONLY:-}"

export BUILD_ROOT DOWNLOAD_DIR JOBS NICE

# systemd starts a service with no HOME, and every toolchain here keeps its cache
# under one: pub, cargo, gradle, npm. Flutter does not degrade when it cannot find
# the pub cache, it refuses to resolve packages at all -- "Could not find the pub
# cache. No `HOME` environment variable exists." -- so the Ambra build failed on
# every timer tick while the login shell it was developed in worked fine. Set it
# here rather than in one recipe: the next toolchain to be added would hit this
# too, and Environment= in the unit would leave a hand-run publish still broken.
export HOME="${HOME:-/root}"

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
  local name="$1" url="$2" ref="$3"
  local dir="$BUILD_ROOT/src/$name"
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
    newest_present="$(find "$DOWNLOAD_DIR" -maxdepth 1 -type f -name "${PRODUCT_INDEX_GLOB%% *}" -printf '%f\n' 2>/dev/null | sort -V | tail -1 || true)"
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

  local recipe name glob newest recipe_version
  for recipe in "$RECIPE_DIR"/*.sh; do
    [ -f "$recipe" ] || continue
    name="$(basename "$recipe" .sh)"
    # Read the glob without running the recipe's build.
    glob="$(grep -m1 '^PRODUCT_INDEX_GLOB=' "$recipe" | cut -d= -f2- | tr -d '"')"
    [ -n "$glob" ] || continue
    recipe_version=""
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
      # The first glob that matched decides the version this product's heading
      # shows. Filenames all carry the same version for a given product, so any
      # of them would do; taking the first keeps it deterministic.
      [ -n "$recipe_version" ] || recipe_version="$(printf '%s' "$newest" | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)"
    done

    # Each product section carries its OWN version, keyed by recipe name. The
    # page used to show one label for everything, which read as if the node's
    # version described Fulmen and Ambra too -- it never did, and the numbers
    # visible on the cards contradicted it. A chip whose data-ver names no
    # recipe is simply left alone.
    if [ -n "$recipe_version" ]; then
      sed -i -E "s#(<span class=\"ver\" data-ver=\"$name\">version )[0-9][0-9.]*#\1$recipe_version#" "$index"
    fi
  done


  # Only what the page actually links: the surrounding prose mentions filenames
  # too ("run Fulmen.exe"), and reporting those as offered artifacts is noise.
  log "page now offers:"
  grep -oE 'href="[^"]+\.(tar\.gz|exe|zip|AppImage|apk)"' "$index" \
    | sed -E 's#href="(.*)"#\1#' | sort -u | sed 's/^/  /'
}

# --- Checksums for everything the directory serves ----------------------------
# One SHA256SUMS over every artifact present, rewritten on every run so it can
# neither name a file that is gone nor miss one that arrived. Built from the
# directory rather than from this run's builds for the same reason the index is:
# a product that was skipped or failed keeps its previous artifact, and that
# artifact still needs a line. The unversioned aliases (ambra-latest.apk and
# friends) are listed too, under their own names, because that is the name a
# download through the stable URL arrives with and `sha256sum -c` matches on the
# name. Verify with: sha256sum --ignore-missing -c SHA256SUMS
write_checksums() {
  local sums="$DOWNLOAD_DIR/SHA256SUMS"
  local -a files=()
  local f
  while IFS= read -r f; do files+=("$f"); done < <(
    find "$DOWNLOAD_DIR" -maxdepth 1 \( -type f -o -type l \) \
      \( -name '*.tar.gz' -o -name '*.exe' -o -name '*.zip' -o -name '*.AppImage' -o -name '*.apk' \) \
      -printf '%f\n' 2>/dev/null | sort -V)
  [ "${#files[@]}" -gt 0 ] || { log "no artifacts to checksum"; return 0; }
  # Complete or not at all, like the artifacts themselves.
  (cd "$DOWNLOAD_DIR" && sha256sum -- "${files[@]}") > "$sums.part"
  mv "$sums.part" "$sums"
  log "SHA256SUMS covers ${#files[@]} file(s)"
}

# --- Signature over the checksums ---------------------------------------------
# A detached OpenPGP signature beside SHA256SUMS, from a key that exists only in
# SIGNING_GNUPGHOME on this host and in its offline backup -- never in any
# repository. The public half is exported beside the signature on every run so a
# verifier has it from the same place, and its fingerprint is pinned in README.md
# in the repository, which is the out-of-band copy to compare against. A host
# with no key publishes the checksums unsigned and says so; the checksums are
# still worth having.
# Verify with: gpg --import sequentia-release-signing-key.asc
#              gpg --verify SHA256SUMS.asc SHA256SUMS
sign_checksums() {
  local sums="$DOWNLOAD_DIR/SHA256SUMS"
  [ -f "$sums" ] || return 0
  local fpr
  fpr="$(GNUPGHOME="$SIGNING_GNUPGHOME" gpg --batch --list-secret-keys --with-colons 2>/dev/null \
         | awk -F: '/^fpr/{print $10; exit}' || true)"
  if [ -z "$fpr" ]; then
    log "no signing key in $SIGNING_GNUPGHOME; SHA256SUMS published unsigned"
    return 0
  fi
  local key="$DOWNLOAD_DIR/sequentia-release-signing-key.asc"
  GNUPGHOME="$SIGNING_GNUPGHOME" gpg --batch --yes --armor --detach-sign --output "$sums.asc.part" "$sums"
  mv "$sums.asc.part" "$sums.asc"
  GNUPGHOME="$SIGNING_GNUPGHOME" gpg --batch --armor --export "$fpr" > "$key.part"
  mv "$key.part" "$key"
  log "SHA256SUMS.asc signed, key $fpr"
}

# --- Go ------------------------------------------------------------------------
[ -d "$RECIPE_DIR" ] || die "no recipes at $RECIPE_DIR"

failed=0
for recipe in "$RECIPE_DIR"/*.sh; do
  [ -f "$recipe" ] || continue
  name="$(basename "$recipe" .sh)"
  # shellcheck disable=SC2086  # ONLY is a space-separated list and must split
  if [ -n "$ONLY" ] && ! printf '%s ' $ONLY | grep -q "$name "; then
    continue
  fi
  # One product failing must not stop the others.
  run_recipe "$recipe" || failed=1
done

update_index
write_checksums
sign_checksums

[ "$failed" = "0" ] || die "at least one product failed to publish (see above)"
log "all products up to date"
