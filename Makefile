accirc: accidental_irc.c
	./compile_accidental_irc.sh

install: accirc
	cp accirc /usr/bin/
	gzip accirc.man
	cp accirc.man.gz /usr/share/man/man1/accirc.1.gz
	gzip -d accirc.man.gz

uninstall:
	rm /usr/bin/accirc
	rm /usr/share/man/man1/accirc.1.gz

clean:
	rm accirc

