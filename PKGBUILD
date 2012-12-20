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
  
  gzip accirc.man
  su -c 'cp accirc.man.gz /usr/share/man/man1/accirc.1.gz'
  su -c 'cp accirc /usr/local/bin/'
}

md5sums=('fac68bbb0d861b59531c746652910da8')
