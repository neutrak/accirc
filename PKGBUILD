# Maintainer: neutrak <neutrak on daemonic.foonetic.net port 6667>
pkgname=accidental_irc
pkgver=0.1
pkgrel=1
pkgdesc="accidental_irc (accirc for short), the accidental multiserver ncurses irc client"
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

package() {
  cd "$srcdir/$pkgname"

  su -c 'cp accirc.man /usr/share/man/man1/'
  su -c 'cp accirc /usr/local/bin/'
  su -c 'ln -s /usr/local/bin/accirc /usr/local/bin/accidental_irc'
}

md5sums=('fac68bbb0d861b59531c746652910da8')
