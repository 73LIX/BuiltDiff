#!/bin/sh
# BuiltDiff installer.
#
#   curl -fsSL https://raw.githubusercontent.com/73LIX/BuiltDiff/refs/heads/main/install.sh | sh

main() {
  set -eu
  REPO="${BUILTDIFF_REPO:-73LIX/BuiltDiff}"
  PREFIX="${BUILTDIFF_PREFIX:-$HOME/.local}"
  VERSION="${BUILTDIFF_VERSION:-latest}"

  say() { printf '%s\n' "$*"; }
  die() { printf 'install.sh: error: %s\n' "$*" >&2; exit 1; }

  case "$REPO" in YOUR_USER/*) die "set BUILTDIFF_REPO=owner/builtdiff (this copy of the script was not customised)";; esac
  case "$REPO" in *[!A-Za-z0-9._/-]*|"") die "invalid BUILTDIFF_REPO";; esac
  case "$VERSION" in *[!A-Za-z0-9._-]*|"") die "invalid BUILTDIFF_VERSION";; esac
  case "$PREFIX" in /*) ;; *) die "BUILTDIFF_PREFIX must be an absolute path";; esac

  [ "$(uname -s)" = "Linux" ] || die "only Linux is supported for now (found $(uname -s)); build from source: see README"
  case "$(uname -m)" in
    x86_64|amd64)  TARGET="linux-x86_64" ;;
    aarch64|arm64) TARGET="linux-aarch64" ;;
    *) die "unsupported CPU $(uname -m); build from source: see README" ;;
  esac

  if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL --proto '=https,http' --tlsv1.2 --retry 2 -o "$2" "$1"; }
  elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -q -O "$2" "$1"; }
  else
    die "need curl or wget"
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    sha() { sha256sum "$1" | cut -d' ' -f1; }
  elif command -v shasum >/dev/null 2>&1; then
    sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
  else
    die "need sha256sum or shasum to verify the download"
  fi

  if [ -n "${BUILTDIFF_BASE_URL:-}" ]; then
    BASE="${BUILTDIFF_BASE_URL%/}"
    [ "$VERSION" != latest ] || die "with BUILTDIFF_BASE_URL set BUILTDIFF_VERSION explicitly"
  elif [ "$VERSION" = latest ]; then
    BASE="https://github.com/$REPO/releases/latest/download"
  else
    BASE="https://github.com/$REPO/releases/download/v${VERSION#v}"
  fi

  TMP=$(mktemp -d "${TMPDIR:-/tmp}/builtdiff-install.XXXXXX") || die "cannot create a temporary directory"
  trap 'rm -rf "$TMP"' EXIT INT TERM

  say "Fetching checksums ..."
  fetch "$BASE/SHA256SUMS" "$TMP/SHA256SUMS" || die "cannot download $BASE/SHA256SUMS (does this release exist?)"

  # The tarball name carries the version; read it from SHA256SUMS instead of guessing.
  ASSET=$(awk -v t="-$TARGET.tar.gz" '{ f=$2; sub(/^\*/, "", f); if (index(f, "builtdiff-") == 1 && substr(f, length(f)-length(t)+1) == t) { print f; exit } }' "$TMP/SHA256SUMS")
  [ -n "$ASSET" ] || die "this release has no build for $TARGET; build from source: see README"
  case "$ASSET" in *[!A-Za-z0-9._-]*) die "unexpected file name in SHA256SUMS";; esac
  WANT=$(awk -v f="$ASSET" '{ n=$2; sub(/^\*/, "", n); if (n == f) { print $1; exit } }' "$TMP/SHA256SUMS")
  case "$WANT" in *[!0-9a-f]*|"") die "malformed SHA256SUMS";; esac
  [ "${#WANT}" -eq 64 ] || die "malformed SHA256SUMS"

  say "Downloading $ASSET ..."
  fetch "$BASE/$ASSET" "$TMP/$ASSET" || die "download failed"
  GOT=$(sha "$TMP/$ASSET")
  [ "$GOT" = "$WANT" ] || die "checksum mismatch (expected $WANT, got $GOT) - not installing"
  say "Checksum OK."

  mkdir "$TMP/x"
  tar -xzf "$TMP/$ASSET" -C "$TMP/x" || die "cannot unpack the archive"
  BIN=$(find "$TMP/x" -type f -name builtdiff | head -n 1)
  [ -n "$BIN" ] && [ -f "$BIN" ] || die "the archive does not contain a builtdiff binary"

  mkdir -p "$PREFIX/bin" || die "cannot create $PREFIX/bin"
  install -m 0755 "$BIN" "$PREFIX/bin/builtdiff.new" || die "cannot write to $PREFIX/bin"
  mv -f "$PREFIX/bin/builtdiff.new" "$PREFIX/bin/builtdiff"

  say "Installed: $PREFIX/bin/builtdiff"
  "$PREFIX/bin/builtdiff" --version || die "the installed binary does not run on this system; build from source: see README"
  case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) say "Note: $PREFIX/bin is not in your PATH. Add this to your shell profile:"; say "  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
  esac
}

main "$@"
