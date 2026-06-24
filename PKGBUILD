# Maintainer: Walther <martinwalther.1989@gmail.com>
_pkgname=vantapaper
pkgname=vantapaper-git
pkgver=r28.21f947c
pkgrel=1
pkgdesc="HDR-correct Hyprland wallpaper daemon with true blacks on OLED"
arch=('x86_64')
url="https://github.com/Waltherion/vantapaper"
license=('GPL-3.0-or-later')
depends=(
  'qt6-base'
  'qt6-wayland'
  'layer-shell-qt'
  'libavif'
  'libjxl'
  'libheif'
  'libultrahdr'
  'lcms2'
  'wayland'
  'vulkan-icd-loader'
)
makedepends=(
  'git'
  'cmake'
  'ninja'
  'qt6-shadertools'
  'wayland-protocols'
  'vulkan-headers'
  'pkgconf'
)
provides=('vantapaper')
conflicts=('vantapaper')
source=("git+$url.git")
sha256sums=('SKIP')

pkgver() {
  cd "$srcdir/$_pkgname"
  printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
  cmake -S "$_pkgname" -B build -G Ninja \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build build
}

package() {
  DESTDIR="$pkgdir" cmake --install build
  install -Dm644 "$_pkgname/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
