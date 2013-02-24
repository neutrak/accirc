# Maintainer: neutrak <neutrak on daemonic.foonetic.net port 6667>
pkgname=accidental_irc
pkgver=0.2
pkgrel=1
pkgdesc="accirc for short; the accidental multiserver ncurses irc client"
arch=('i686' 'x86_64')
url=""
license=('LGPLv3')
groups=()
depends=('ncurses>=5.9-3')
makedepends=('gcc')
source=(http://somegenericsite.dyndns.org/code/$pkgname/$pkgname-$pkgver.tar.gz)
# md5sums=() #generate with 'makepkg -g'

build() {
  cd "$srcdir/$pkgname"
  
  ./compile_accidental_irc.sh
}

#for the moment, don't install; just d/l and compile
#package() {
#  cd "$srcdir/$pkgname"
#  
#  gzip accirc.man
#  cp accirc.man.gz /usr/share/man/man1/accirc.1.gz
#  cp accirc /usr/local/bin/
#}

md5sums=('b60d7793e20721ddd9c11a446c0f3b21')
