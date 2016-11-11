#!/bin/bash

#this has been tested with gcc; other compilers may or may not work
#this code should meet default gcc standard and gnu89 standard
#the $* is for -D DEBUG and similar

#if no C compiler is set in the environmental variables just try gcc
if [ -z "${CC}" ]
then
	CC="gcc"
fi

echo "Compiling using ${CC}..."

#$CC -o accirc accidental_irc.c -Wall -lncurses $*

#unicode support requires linking against ncursesw
$CC -o accirc accidental_irc.c -I/usr/include/ncursesw -Wall -lncursesw $*

if [ 0 -eq "$?" ]
then
	echo "Compiled!"
fi

