#!/bin/bash

#the $* is for -D DEBUG and similar
gcc -g -o accirc accidental_irc.c -Wall -lncurses $*

