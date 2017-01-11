#!/usr/bin/env python3

#this script formats accirc irc logs in a prettier way
#note that this assumes the file is utf-8 encoded; it will choke on latin-1 and other non-ascii single-byte encodings

import sys
import datetime

def pretty_format_line(line):
	#ignore blank lines
	if(len(line)==0):
		return ''
	space_idx=line.find(' ')
	
	#timestamp
	try:
		ts=int(line[0:space_idx])
	except ValueError:
		return line
	
	line=line[space_idx+1:]
	
	#messages sent by the user have no special meaning in pretty-format
	if(line.startswith('>> ')):
		line=line[len('>> '):]
	
	#control messages have no special meaning
	if(line.startswith(':')):
		line=line[len(':'):]
	
	#pings have no special meaning
	if(line.startswith('***')):
		line=line[len('***'):]
	
	return datetime.datetime.fromtimestamp(ts).strftime('%Y-%m-%d %R:%S')+' '+line
	

def pretty_format_file(fname):
	fp=open(fname,'r')
	fcontent=fp.read()
	fp.close()
	
	ret=''
	lines=fcontent.split("\n")
	for line in lines:
		ret+=pretty_format_line(line)+"\n"
	return ret

if(__name__=='__main__'):
	if(len(sys.argv)>1):
		print(pretty_format_file(sys.argv[1]))
	else:
		print('Usage: '+sys.argv[0]+' <accirc log file>')

