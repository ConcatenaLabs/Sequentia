# Ambra: the Flutter mobile wallet, published as a signed Android APK.
#
# This recipe is complete but will SKIP itself until two things exist on the box,
# and both are deliberate decisions rather than oversights:
#
#   1. A Flutter + Android SDK/NDK + Rust toolchain. ambra_core is Rust compiled
#      to .so libraries that the Flutter app loads, so the Rust half must be built
#      too, not just the Dart half.
#
#   2. A release signing keystore. An Android release APK is signed with a key
#      that IS the app's identity: users' phones refuse an update signed by a
#      different key, so losing it or leaking it cannot be undone by rotating it.
#      Today that key lives on the laptop. Putting it on a server that also runs
#      a public node is a decision for whoever owns the key, not something this
#      script should quietly arrange, so it is read from a path that does not
#      exist by default and the product is skipped until it does.
#
# Debug-signed APKs are NOT published. An APK signed with a debug key installs
# but can never be upgraded by a real release, so shipping one would strand every
# user who installed it.
PRODUCT_NAME="ambra"
PRODUCT_REPO="${SEQ_AMBRA_REPO:-https://github.com/GracedEternalKingCabbageMan/ambra.git}"
PRODUCT_INDEX_GLOB="ambra-*.apk"

# Empty means "ask the remote what its default branch is".
AMBRA_BRANCH="${SEQ_AMBRA_BRANCH:-}"
branch() { echo "${AMBRA_BRANCH:-$(default_branch "$PRODUCT_REPO")}"; }
# Point these at the real key to enable the product.
AMBRA_KEYSTORE="${SEQ_AMBRA_KEYSTORE:-/etc/sequentia/ambra-release.keystore}"
AMBRA_KEY_PROPERTIES="${SEQ_AMBRA_KEY_PROPERTIES:-/etc/sequentia/ambra-key.properties}"

requirements() {
  have flutter || { echo "flutter not installed"; return 1; }
  have cargo   || { echo "rust toolchain not installed (ambra_core is Rust)"; return 1; }
  have java    || { echo "java not installed (Android build)"; return 1; }
  [ -n "${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}" ] || { echo "ANDROID_SDK_ROOT not set"; return 1; }
  [ -f "$AMBRA_KEYSTORE" ] || { echo "no release keystore at $AMBRA_KEYSTORE (see the note in this recipe)"; return 1; }
  [ -f "$AMBRA_KEY_PROPERTIES" ] || { echo "no key.properties at $AMBRA_KEY_PROPERTIES"; return 1; }
  return 0
}

# NOTE, before enabling this product: app/pubspec.yaml currently reads 0.13.7
# while the download page already offers ambra-0.16.4.apk, so the APKs published
# so far were versioned from somewhere other than pubspec. Confirm which is the
# real source before turning this on. Until then the driver's downgrade guard
# refuses to publish 0.13.7 over 0.16.4, which is the safe outcome but not a
# substitute for knowing the answer.
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

  # The Rust core first: the Flutter build bundles whatever .so files are already
  # staged, so building Dart against a stale core is a silent, shipped bug.
  log "[ambra] building ambra_core (Rust)"
  ( cd "$dir/ambra_core" && nice -n "$NICE" bash -c './build-android.sh' ) \
    || { log "[ambra] ambra_core build failed"; return 1; }

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
