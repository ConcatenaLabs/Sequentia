# Ambra: the Flutter mobile wallet, published as a signed Android APK.
#
# ambra_core is Rust compiled to .so libraries that the Flutter app loads, so the
# Rust half is built first: building Dart against a stale core is a silent bug
# that ships.
#
# The signing key IS the app's identity -- a phone refuses an update signed by a
# different key, so a lost or rotated key strands every existing user. It lives at
# /etc/sequentia, root-only, and never in git.
#
# Debug-signed APKs are NOT published. Such a build installs and can then never be
# upgraded by a real release, stranding whoever installed it.
PRODUCT_NAME="ambra"
PRODUCT_REPO="${SEQ_AMBRA_REPO:-https://github.com/GracedEternalKingCabbageMan/ambra.git}"
PRODUCT_INDEX_GLOB="ambra-*.apk"

# Toolchain, as installed on the box. Exported rather than assumed to be on PATH,
# because the timer runs this with systemd's environment, not a login shell's.
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export PATH="/opt/flutter/bin:/root/.cargo/bin:$ANDROID_SDK_ROOT/platform-tools:$PATH"

# Empty means "ask the remote what its default branch is".
AMBRA_BRANCH="${SEQ_AMBRA_BRANCH:-}"
branch() { echo "${AMBRA_BRANCH:-$(default_branch "$PRODUCT_REPO")}"; }
# Point these at the real key to enable the product.
SWK_REPO="${SEQ_SWK_REPO:-https://github.com/GracedEternalKingCabbageMan/SWK.git}"
SEQLN_REPO="${SEQ_SEQLN_REPO:-https://github.com/GracedEternalKingCabbageMan/seqln.git}"
AMBRA_KEYSTORE="${SEQ_AMBRA_KEYSTORE:-/etc/sequentia/ambra-release.keystore}"
AMBRA_KEY_PROPERTIES="${SEQ_AMBRA_KEY_PROPERTIES:-/etc/sequentia/ambra-key.properties}"

requirements() {
  have flutter || { echo "flutter not on PATH (expected /opt/flutter/bin)"; return 1; }
  have cargo   || { echo "cargo not on PATH (expected /root/.cargo/bin); ambra_core is Rust"; return 1; }
  have java    || { echo "java not installed"; return 1; }
  [ -d "$ANDROID_SDK_ROOT/platform-tools" ] || { echo "no Android SDK at $ANDROID_SDK_ROOT"; return 1; }
  # The key cannot be generated or recovered here: it IS the app's identity, and
  # a different one strands every user who already installed Ambra. So this is
  # the one thing the box must be given rather than install for itself.
  [ -f "$AMBRA_KEYSTORE" ] || { echo "no release keystore at $AMBRA_KEYSTORE"; return 1; }
  [ -f "$AMBRA_KEY_PROPERTIES" ] || { echo "no key.properties at $AMBRA_KEY_PROPERTIES"; return 1; }
  return 0
}

remote_version() {
  local dir="$BUILD_ROOT/src/ambra" br; br="$(branch)"
  [ -n "$br" ] || return 1
  if [ ! -d "$dir/.git" ]; then
    git clone -q "$PRODUCT_REPO" "$dir" >/dev/null 2>&1 || return 1
  fi
  git -C "$dir" fetch -q origin "$br" || return 1
  # pubspec's "version: 1.2.3+45" -- the part before the build number is what the
  # download page has always used.
  git -C "$dir" show "origin/$br:app/pubspec.yaml" 2>/dev/null \
    | awk '/^version:/ {print $2; exit}' | cut -d+ -f1
}

build() {
  local version="$1" out="$2" br; br="$(branch)"
  local dir; dir="$(prepare_checkout ambra "$PRODUCT_REPO" "origin/$br")"

  # ambra_core does not depend on published crates for the Sequentia-aware parts:
  # ambra_core/Cargo.toml patches lwk_common, lwk_signer, lwk_wollet and elements
  # to ../../SWK/*, the unpublished wallet-kit fork whose rust-elements can
  # (de)serialize this chain's anchored block headers. Those are PATH patches, so
  # SWK has to exist as a sibling of the ambra checkout or cargo stops at
  # "failed to load source for dependency `elements`". prepare_checkout puts every
  # product under $BUILD_ROOT/src/<name>, which is exactly that layout.
  # seqln is the second one: ambra_core takes seqln-signer from
  # ../../seqln/contrib/seqln-signer by path as well.
  #
  # Each is checked out and then CONFIRMED by the directory cargo will actually
  # read. A checkout that half-succeeds leaves a clone whose contents are missing,
  # and the failure then surfaces from cargo as a puzzling "failed to load source
  # for dependency", naming the crate rather than the checkout that never happened.
  sibling() {  # sibling <name> <repo> <path cargo needs, relative to the clone>
    local name="$1" repo="$2" probe="$3" br
    br="${SEQ_SIBLING_BRANCH:-$(default_branch "$repo")}"
    [ -n "$br" ] || { log "[ambra] cannot resolve $name's default branch"; return 1; }
    log "[ambra] preparing $name ($br) for the path-patched crates"
    prepare_checkout "$name" "$repo" "origin/$br" >/dev/null \
      || { log "[ambra] could not check out $name"; return 1; }
    [ -e "$BUILD_ROOT/src/$name/$probe" ] \
      || { log "[ambra] $name checked out but $probe is missing; cargo needs it"; return 1; }
  }

  sibling SWK   "$SWK_REPO"   "rust-elements/Cargo.toml"          || return 1
  sibling seqln "$SEQLN_REPO" "contrib/seqln-signer/Cargo.toml"   || return 1

  # The Rust core first. app/android/app/src/main/jniLibs is gitignored and never
  # committed, so the Flutter build bundles whatever .so happens to be staged --
  # building Dart against a stale core is a silent bug that ships. Building into
  # a clean checkout each time is what makes that impossible here.
  log "[ambra] cross-compiling ambra_core for Android"
  ( cd "$dir/ambra_core" \
      && nice -n "$NICE" cargo ndk -t arm64-v8a \
           -o ../app/android/app/src/main/jniLibs build --release ) \
    || { log "[ambra] ambra_core build failed"; return 1; }
  [ -n "$(ls -A "$dir/app/android/app/src/main/jniLibs" 2>/dev/null)" ] \
    || { log "[ambra] no jniLibs produced; the APK would ship without its core"; return 1; }

  log "[ambra] building the signed release APK"
  cp "$AMBRA_KEY_PROPERTIES" "$dir/app/android/key.properties"
  ( cd "$dir/app" \
      && nice -n "$NICE" flutter pub get >/dev/null \
      && nice -n "$NICE" flutter build apk --release ) \
    || { log "[ambra] flutter build failed"; return 1; }

  local apk="$dir/app/build/app/outputs/flutter-apk/app-release.apk"
  [ -f "$apk" ] || { log "[ambra] no release APK at $apk"; return 1; }

  # Refuse to publish something signed with a debug key: it installs, and then no
  # real release can ever upgrade it.
  if have apksigner && apksigner verify --print-certs "$apk" 2>/dev/null | grep -qi 'CN=Android Debug'; then
    log "[ambra] APK is debug-signed; refusing to publish"
    return 1
  fi

  cp "$apk" "$out/ambra-$version.apk"
}
