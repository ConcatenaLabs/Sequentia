# shellcheck shell=bash
# shellcheck disable=SC2034  # PRODUCT_* are read by the driver, not used here
# Pignus: the collateralised-lending CLI, oracle, liquidator and daemon.
#
# Versioned by __version__ in pignus/__init__.py, the number `pignus-cli
# --version` reports, so it is the one a user can compare against what they
# have. Pure Python with no build step: the artifact is the repository, minus
# its development files, as a source tarball. It needs a Sequentia source
# checkout at run time (the loan covenant is imported from the node repository),
# which the README inside explains.
# Sorting and version comparison below are locale-sensitive.
export LC_ALL=C

PRODUCT_NAME="pignus"
PRODUCT_REPO="${SEQ_PIGNUS_REPO:-https://github.com/ConcatenaLabs/pignus.git}"
PRODUCT_INDEX_GLOB="pignus-cli-*.tar.gz"

# Empty means "ask the remote what its default branch is".
PIGNUS_BRANCH="${SEQ_PIGNUS_BRANCH:-}"

requirements() {
  have git || { echo "git missing"; return 1; }
  have tar || { echo "tar missing"; return 1; }
  return 0
}

branch() { echo "${PIGNUS_BRANCH:-$(default_branch "$PRODUCT_REPO")}"; }

remote_version() {
  # Read the version from the branch tip without a checkout, so the cheap
  # "has anything changed" path stays cheap.
  local dir="$BUILD_ROOT/src/pignus" br; br="$(branch)"
  [ -n "$br" ] || return 1
  if [ ! -d "$dir/.git" ]; then
    git clone -q "$PRODUCT_REPO" "$dir" >/dev/null 2>&1 || return 1
  fi
  git -C "$dir" fetch -q origin "$br" || return 1
  git -C "$dir" show "origin/$br:pignus/__init__.py" 2>/dev/null \
    | sed -nE 's/^__version__ *= *"([^"]+)".*/\1/p' | head -1
}

build() {
  local version="$1" out="$2" br; br="$(branch)"
  local dir; dir="$(prepare_checkout pignus "$PRODUCT_REPO" "origin/$br")"
  cd "$dir" || return 1

  # What a user downloads must not carry our development scaffolding: CLAUDE.md
  # is instructions to an agent, .claude/ is agent state, and .git would
  # multiply the size for nothing. tests/ stays in: the README tells a new
  # user to run tests/cli_drill.sh to check the wiring.
  # The top-level directory is pignus-<version>/, which is what the page's
  # instructions unpack.
  local tarball="$out/pignus-cli-$version.tar.gz"
  nice -n "$NICE" tar czf "$tarball" \
    --exclude-vcs --exclude='./CLAUDE.md' --exclude='./.claude' \
    --exclude='__pycache__' --exclude='*.pyc' \
    --transform "s,^\\.,pignus-$version," .

  [ -s "$tarball" ] || { log "[pignus] tarball produced nothing"; return 1; }
  # A tarball without the CLI or the covenant vectors is not a release.
  local listing; listing="$(tar tzf "$tarball")"
  for must in "pignus-$version/bin/pignus-cli" "pignus-$version/pignus/vectors.json" \
              "pignus-$version/README.md"; do
    printf '%s\n' "$listing" | grep -qx "$must" \
      || { log "[pignus] tarball lacks $must"; return 1; }
  done
  # Every Python entry point must at least compile.
  python3 -m py_compile bin/pignus-cli bin/pignusd bin/pignus-oracle bin/pignus-liquidator \
    || { log "[pignus] an entry point does not compile"; return 1; }
}
