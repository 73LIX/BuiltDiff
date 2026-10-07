#!/bin/sh
# release-build.sh VERSION
#
# Builds a fully static musl builtdiff for linux-x86_64 and linux-aarch64 and
# packs the tarballs install.sh expects:
#
#   dist/builtdiff-$VERSION-linux-x86_64.tar.gz
#   dist/builtdiff-$VERSION-linux-aarch64.tar.gz
#   dist/SHA256SUMS
#
# Toolchains come from musl.cc (https://musl.cc/*-linux-musl-cross.tgz) and run
# natively on any Linux host: the end user never needs Docker or any toolchain,
# and neither does this machine. The compilers are cached in
# $XDG_CACHE_HOME/builtdiff-toolchain (override with BUILTDIFF_TC_CACHE).
#
# Before tagging, bump kToolVersion in src/util.hpp to match VERSION; the script
# refuses to package a binary that reports a different version.
#
# Env overrides (used by CI and tests):
#   BUILTDIFF_TC_CACHE   toolchain cache dir (default $XDG_CACHE_HOME or ~/.cache)
#   BUILTDIFF_DIST       where the tarballs land (default <repo>/dist)
#   BUILTDIFF_ARCHES     space-separated arches (default "x86_64 aarch64")
#   BUILTDIFF_SOURCED=1  import the functions without running anything

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DIST="${BUILTDIFF_DIST:-$REPO_ROOT/dist}"
CACHE="${BUILTDIFF_TC_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/builtdiff-toolchain}"
ARCHES="${BUILTDIFF_ARCHES:-x86_64 aarch64}"
CMAKEFLAGS="-G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILTDIFF_BUILD_TESTS=OFF"

need() { command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"; }
die() { printf 'release-build.sh: error: %s\n' "$*" >&2; exit 1; }

# Target prefix used by the musl.cc compilers and the sysroot inside the tree:
# the toolchain directory adds a "-cross" suffix ("x86_64-linux-musl-cross"),
# but the g++/gcc binaries and the sysroot subdirectory use the bare target
# ("x86_64-linux-musl"). Mixing the two up points CMake at a compiler that
# does not exist.
target_of() { case "$1" in x86_64) printf '%s' x86_64-linux-musl ;; aarch64) printf '%s' aarch64-linux-musl ;; esac; }

fetch_toolchain() {  # arch
  target=$(target_of "$1")
  [ -n "$target" ] || die "unsupported arch: $1"
  tcname="$target-cross"
  if [ -x "$CACHE/$tcname/bin/$target-g++" ]; then return; fi
  mkdir -p "$CACHE"
  need curl
  tarball="$CACHE/$tcname.tgz"
  printf 'Fetching %s toolchain ...\n' "$tcname"
  curl -fsSL --retry 2 -o "$tarball" "https://musl.cc/$tcname.tgz"
  tmp="$CACHE/.unpack-$tcname"
  rm -rf "$tmp"; mkdir -p "$tmp"
  tar -xzf "$tarball" -C "$tmp"
  found=
  for d in "$tmp"/*/; do [ -d "$d/bin" ] && found=1 && mv -- "$d" "$CACHE/$tcname" && break; done
  rm -rf "$tmp" "$tarball"
  [ -n "$found" ] || die "toolchain archive did not unpack as expected"
}

build_one() {  # arch
  arch=$1
  target=$(target_of "$arch")
  tcname="$target-cross"
  tc="$CACHE/$tcname"
  sdk="$tc/$target"          # the sysroot sits next to bin/ inside the archive
  builddir="$REPO_ROOT/build-release-$arch"
  rm -rf "$builddir"
  cmake -S "$REPO_ROOT" -B "$builddir" $CMAKEFLAGS \
    -DCMAKE_CXX_COMPILER="$tc/bin/$target-g++" \
    -DCMAKE_SYSROOT="$sdk" \
    -DCMAKE_FIND_ROOT_PATH="$sdk" \
    -DCMAKE_EXE_LINKER_FLAGS="-static"
  cmake --build "$builddir"
  bin="$builddir/builtdiff"
  [ -f "$bin" ] || die "build produced no binary for $arch"
  # Strip with the toolchain's own strip: it knows the target format for aarch64
  # cross builds, where the host strip would fail.
  "$tc/bin/$target-strip" "$bin" 2>/dev/null || true
  package_one "$VERSION" "$arch" "$bin"
}

package_one() {  # version arch binary
  ver=$1; arch=$2; bin=$3
  [ -x "$bin" ] || die "not an executable: $bin"
  mkdir -p "$DIST"

  # The arch matches the host? Then the binary can be executed and must agree
  # on the version. Cross binaries cannot run here (and never will by users).
  case "$arch" in
    "$(uname -m)") ;;
    x86_64) [ "$(uname -m)" = amd64 ] || skip=1 ;;
    *) skip=1 ;;
  esac
  if [ -z "${skip:-}" ]; then
    "$bin" --version 2>/dev/null | grep -qx "builtdiff $ver" || die \
      "binary reports a version other than $ver; bump kToolVersion in src/util.hpp first"
  fi

  if command -v file >/dev/null 2>&1; then
    case "$(file "$bin")" in
      *"statically linked"*|*"static-pie linked"*) ;;
      *) printf 'release-build.sh: warning: %s does not look statically linked\n' "$bin" >&2 ;;
    esac
  fi

  stage="$DIST/.stage-$arch"
  rm -rf "$stage"; mkdir -p "$stage"
  install -m 0755 "$bin" "$stage/builtdiff"
  tar -C "$stage" -czf "$DIST/builtdiff-$ver-linux-$arch.tar.gz" builtdiff
  rm -rf "$stage"
  printf 'packed %s\n' "$DIST/builtdiff-$ver-linux-$arch.tar.gz"
}

make_checksums() {
  # sha256sum output is "hash  filename"; install.sh parses both columns.
  if command -v sha256sum >/dev/null 2>&1; then
    ( cd "$DIST" && sha256sum builtdiff-*.tar.gz > SHA256SUMS )
  elif command -v shasum >/dev/null 2>&1; then
    ( cd "$DIST" && shasum -a 256 builtdiff-*.tar.gz > SHA256SUMS )
  else
    die "need sha256sum or shasum"
  fi
  printf 'wrote %s\n' "$DIST/SHA256SUMS"
}

main() {
  VERSION="${1:-}"
  case "$VERSION" in ''|*[!0-9.]*) die "usage: $0 VERSION  (e.g. 1.0.0)";; esac
  need cmake; need ninja; need tar
  for arch in $ARCHES; do
    fetch_toolchain "$arch"
    build_one "$arch"
  done
  make_checksums
  printf 'done. attach dist/*.tar.gz and dist/SHA256SUMS to the GitHub release.\n'
}

# Import-only mode for tests and CI helpers.
if [ "${BUILTDIFF_SOURCED:-0}" != 1 ] && [ "$(basename -- "$0")" = "release-build.sh" ]; then
  main "$@"
fi