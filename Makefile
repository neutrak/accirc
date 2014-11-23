accirc: accidental_irc.c
	./compile_accidental_irc.sh -lssl -lcrypto -D _OPENSSL

nossl: accidental_irc.c
	./compile_accidental_irc.sh

debug: accidental_irc.c
	./compile_accidental_irc.sh -g -lssl -lcrypto -D _OPENSSL -D DEBUG

install: accirc
	mkdir -p $(DESTDIR)/usr/bin/
	cp accirc $(DESTDIR)/usr/bin/
	gzip accirc.man
	mkdir -p $(DESTDIR)/usr/share/man/man1/
	cp accirc.man.gz $(DESTDIR)/usr/share/man/man1/accirc.1.gz
	gzip -d accirc.man.gz

uninstall:
	rm $(DESTDIR)/usr/bin/accirc
	rm $(DESTDIR)/usr/share/man/man1/accirc.1.gz

clean:
	rm accirc

