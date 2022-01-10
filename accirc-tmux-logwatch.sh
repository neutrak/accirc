#!/bin/bash

#this is a one-liner shell script that lets me keep an eye on pings via tmux and watch
truncate -s 0 ~/.local/share/accirc/logs/pings.txt && watch -n 5 'tmux rename-window -t 1 "accirc-pings ($(cat ~/.local/share/accirc/logs/pings.txt | wc -l))" ; tail -n 10 ~/.local/share/accirc/logs/pings.txt'

