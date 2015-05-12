accirc (aka accidental_irc) README.txt
accidental irc is an ncurses-based irc client; it is written in gnu89-compliant C, and should compile and run on any system with gcc and ncurses
see the manual page for usage; these are compiler and installation instructions

Compiling and installation instructions:
	For the most basic compilation, just use the existing Make script
		Note that for this to work you will need ncursesw and openssl headers installed
		
		make
	
	To use a compiler other than gcc, change the environment var prior to make; for example clang can be used with
		
		CC=clang make
	
	To build without ssl, use the following
		
		make nossl
	
	Once built, install with make install; THIS REQUIRES ROOT
	
		make install
	

Uninstallation:
	If for any reason you would like to uninstall this program, you can do it with the following command
	
		make uninstall


Running after installation:
	It's just
		
		accirc
	
	To run without installing (from this directory)
		
		./accirc
	
	The switches and --ignorerc --proper are supported; read the man page for detailed options and commands
	To view the man page (after installation) use
		
		man accirc
	
	To view the man page without installing (from this directory) use
		
		man ./accirc.man




