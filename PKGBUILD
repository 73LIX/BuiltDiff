# Maintainer: you <you@example.com>
pkgname=builtdiff
pkgver=0.1.0
pkgrel=1
pkgdesc="Explains why a project builds/runs on the developer's machine but not on yours (optional local Gemma analysis)"
arch=('x86_64' 'aarch64')
license=('MIT')
depends=('gcc-libs')
optdepends=('ollama: local Gemma model for "check --analyze"')
makedepends=('cmake' 'ninja')

build() {
  cmake -S "$startdir" -B "$srcdir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr -DBUILTDIFF_BUILD_TESTS=OFF
  cmake --build "$srcdir/build"
}

package() {
  DESTDIR="$pkgdir" cmake --install "$srcdir/build"
  install -Dm644 "$startdir/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
