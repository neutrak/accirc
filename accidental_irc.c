//accidental irc, the accidental multi-server ncurses irc client
//in C at the moment (a rewrite will be done if I ever finish writing the interpreter for neulang...)

//this takes a fractal design as follows
//	client has set of connections
//		connection has set of channels

//libraries
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
//signals
#include <signal.h>
//ncurses
#include <ncurses.h>
//networking
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
//ssl support via openssl, enable with -D OPENSSL
//(left as a configuration option for platform support on devices that don't have those libs)
#ifdef _OPENSSL
	#include <openssl/rand.h>
	#include <openssl/ssl.h>
	#include <openssl/err.h>
	#include <openssl/x509_vfy.h>
#endif
//non-blocking
#include <fcntl.h>
#include <errno.h>
//time
#include <sys/time.h>
#include <time.h>
//utf-8
#include <locale.h>

//doubly-linked lists
#include "dlist.h"

//preprocessor defines
#define TRUE 1
#define FALSE 0

//the smallest terminal we can actually use (smaller than this and we exit with error)
#define MIN_HEIGHT 7
#define MIN_WIDTH 12

//switching to more typical semantic major.minor.patch versioning as of version 2.0; previous version was "0.13.1"
//I'm counting at 2.0.0 rather than 1.0.0 for two reasons:
//	a) the doubly-linked list library change counts as a substantial rewrite
//	b) it's not the first public release so version 1 doesn't really make sense
//hence going forward we'll be on 2.x.x for a bit
#define VERSION "2.0.3"

//these are for ncurses' benefit
#define KEY_ESCAPE 27
#define KEY_DEL KEY_DC
#define BREAK 0x03
//(I know the IRC spec limits it to 512 but I'd like to have some extra room in case of client commands or something)
#define BUFFER_SIZE 1024

//NOTE: dlist is a linked list library that operates on void* and is used for servers, channels, name lists, aliases, ping phrases, and post lines
//so that we don't have to set a fixed limit and so that when we're not using a large number of those we use less memory

//because we now allow arbitrary-length server lists and channel lists,
//when an overflow occurs where the entire list could not fit in one line (but not before), the following display is used:
//	the active server or channel should be centered in the display
//	in the case that there are more servers or channels that cannot be completely displayed, there will be arrows displayed, "<- " on the left and/or "->" on the right
//		the arrows themselves will be highlighted based on the highest-priority alert/ping state from any server or channel that is out of view
//		e.g. when a ping occurs on a server that is scrolled out of view, the arrow will be bold and underlined (just as the server or channel name would be)

//the number of lines of scrollback to store (per channel, and for input history) (each line being BUFFER_SIZE chars long)
#define MAX_SCROLLBACK 512

//for MIRC colors (these are indexes in an array)
#define FOREGROUND 0
#define BACKGROUND 1

#define REFRESH_DIRECTION_LEFT -1
#define REFRESH_DIRECTION_RIGHT 1

//some defaults in case the user forgets to give a nickname
#define DEFAULT_NICK "accirc_user"

//and in case the user doesn't give a proper quit message
#define DEFAULT_QUIT_MESSAGE "accidental_irc exited"

//for the SIGHUP handler
#define DEFAULT_TERM_LOST_MESSAGE "accidental_irc exited (lost terminal)"

//default leading characters to send raw data and run client commands, respectively
#define DEFAULT_SERVER_ESCAPE ':'
#define DEFAULT_CLIENT_ESCAPE '/'

//the error to display when a line overflows
#define LINE_OVERFLOW_ERROR "<<etc.>>"

//the default directory to save logs to
#define LOGGING_DIRECTORY "logs"

//the file to log errors to
#define ERROR_FILE "error_log.txt"

//the number of seconds to try to reconnect before giving up
#define RECONNECT_TIMEOUT 5

//how many completion attempts to give the user before telling them the possiblities
#define COMPLETION_ATTEMPTS 2

//how many user names to output when resolving non-unique tab completions
#define MAX_OUTPUT_NICKS 8

//how much time (in seconds) to wait on a server to say something
//before we just figure it's dead and disconnect
//(and perhaps try to reconnect, depending on runtime state)
#define SERVER_TIMEOUT 1200

//how many lines are reserved for something other than the channel text
//one for server list
//one for channel list
//one for channel topic
//one for upper delimeter
//one for lower delimter and time
//one for input area
#define RESERVED_LINES 6

//a list of commands for interactive help; the man page is of course the real source for all help, this is just a summary
char *command_list[]={
	": -> send raw data (server commands)",
	"/help -> displays short help (READ THE MAN PAGE FOR REAL DOCS!!!); this is not always kept up to date, and it is just a summary",
	"/connect <host> <port> -> connects to a server",
#ifdef _OPENSSL
	"/sconnect <host> <port> -> connects to a server with SSL",
#endif
	"/exit -> closes all connections and closes program",
	"/cli_escape <new> -> changes client command escape character, used for these commands (default \'/\')",
	"/ser_escape <new> -> changes server escape (default \':\')",
	"/alias <trigger> <sub> -> does text substitution on /trigger to act like the given substitution; if <sub> is omitted and the trigger previously existed the alias will be deleted",
	"/time_format <timestring> -> changes clock display as requested",
	"/set_version <version string> -> sets a custom string for CTCP VERSION",
	"/set_quit_msg <quit message> -> sets a custom quit message for the current server",
	"/easy_mode -> turns easy_mode on, which auto-sends user and nick info to new servers",
	"/no_easy_mode -> turns easy_mode off",
	"/center_server_list -> displays only a single server name (and arrows) even when there is room to display them all",
	"/no_center_server_list -> displays all server names left to right if there's room, falls back to centered when there's no room left",
	"/reconnect -> reconnect to the current server if dropped",
	"/no_reconnect -> don't reconnect to the current server if dropped",
	"/manual_reconnect -> force a reconnect even if a drop was not detected",
	"/log -> keep logs for current server",
	"/no_log -> don't keep logs for current server",
	"/rsearch -> text search up in the buffer",
	"/up -> scrolls up one line",
	"/down -> scrolls down one line",
	"/head -> scrolls to first line in buffer",
	"/tail -> scrolls to last line in buffer",
	"/swcl -> swap current channel for channel to the left of it in the channel list",
	"/swcr -> swap current channel for channel to the right of it in the channel list",
	"/swsl -> swap current server for server to the left of it in the server list",
	"/swsr -> swap current server for server to the left of it in the server list",
	"/hi <nick> -> opens a faux channel for PM-ing with the user named <nick>",
	"/bye -> closes the actively selected faux-pm channel",
	"/sl -> server left (aka f3, alt+up)",
	"/sr -> server right (aka f4, alt+down)",
	"/cl -> channel left (aka f1, alt+right)",
	"/cr -> channel right (aka f2, alt+right)",
	"/refresh [left|right] -> cycle to next unread channel or server/channel combination (default in left->right direction), can loop, if no unreads cycles all channels (aka f5 and f6 for right->left and left->right respectively)",
	"/me -> sends CTCP ACTION message",
	"/r -> replies by pm to last user who PM'd you (or empty string if no PMs recieved)",
	//reverse is an easter egg and so is not documented
	//morse and unmorse aren't documented here for brevity, but are documented in the manual page
	"/set_channel_order [#channel]+ -> defines the order in which joined channels should display; channels not included in this list will be displayed in the order they were joined",
	"/hide_user_mode_changes <user modes to hide> -> makes the given user mode changes on the selected server not output in the console; set to empty string to make all user mode changes visible (default)",
	"/usleep <microseconds> -> pauses program for given microsecond count",
	"/comment -> ignores this line (for rc files)",
	"/fallback_nick <nick> -> sets nick if other nick taken",
	"/post <server command> -> causes commands after this to be delayed until after server command received (for rc files)",
	"/no_post -> stops listening for commands to delay",
	"/rejoin_on_kick -> automatically rejoins channels on current server if kicked",
	"/no_rejoin_on_kick -> doesn't automatical rejoin channels on current server if kicked (default)",
	"/mode_str -> displays modes associated with each user on PRIVMSG including channel messages on current server",
	"/no_mode_str -> doesn't display modes associated with each user on PRIVMSG including channel messages on current server (default)",
	"/ping_toggle <phrase> -> toggles whether or not a PING is done on a given phrase",
	"/auto_hi -> automatically creates a faux channel when a user PMs you (default)",
	"/no_auto_hi -> disables automatic faux channel creation when a user PMs you",
#ifdef _OPENSSL
	"/ssl_cert_check -> enable ssl certificate verification on all new ssl connections (default)",
	"/no_ssl_cert_check -> disable ssl certificate verification on all new ssl connections [DANGEROUS!]",
#endif
	"/ping_on_pms -> makes PMs in faux PM channels on the current server considered PINGs",
	"/no_ping_on_pms -> makes PMs in faux PM channels on the server considered as normal messages after the first one (default)",
	"/hide_joins_quits -> hides all JOINs, QUITs, and PARTs from what would otherwise be normal channel output; does not affect user list",
	"/no_hide_joins_quits -> shows all JOINs, QUITs, and PARTs in normal channel output (default)",
	"/disconnect -> disconnects from the currently active server without sending a QUIT (resulting in an I/O error)",
	"<Tab> -> automatically completes nicks in current channel"
};

enum {
	MIRC_WHITE,
	MIRC_BLACK,
	MIRC_BLUE,
	MIRC_GREEN,
	MIRC_RED,
	MIRC_BROWN,
	MIRC_PURPLE,
	MIRC_ORANGE,
	MIRC_YELLOW,
	MIRC_LIGHT_GREEN,
	MIRC_TEAL,
	MIRC_LIGHT_CYAN,
	MIRC_LIGHT_BLUE,
	MIRC_PINK,
	MIRC_GREY,
	MIRC_LIGHT_GREY,
	
	MIRC_COLOR_MAX
};

//an array of all morse characters starting at a
char *morse_chars[]={
	".-", //A
	"-...", //B
	"-.-.", //C
	"-..", //D
	".", //E
	"..-.", //F
	"--.", //G
	"....", //H
	"..", //I
	".---", //J
	"-.-", //K
	".-..", //L
	"--", //M
	"-.", //N
	"---", //O
	".--.", //P
	"--.-", //Q
	".-.", //R
	"...", //S
	"-", //T
	"..-", //U
	"...-", //V
	".--", //W
	"-..-", //X
	"-.--", //Y
	"--..", //Z
};

//an array of all morse numbers starting at 0
char *morse_nums[]={
	"-----", //0
	".----", //1
	"..---", //2
	"...--", //3
	"....-", //4
	".....", //5
	"-....", //6
	"--...", //7
	"---..", //8
	"----.", //9
};

//structures


//a NICK struct that includes both user name and mode string so we don't have a weird parallel array setup
typedef struct nick_info nick_info;
struct nick_info {
	char user_name[BUFFER_SIZE];
	char mode_str[BUFFER_SIZE];
};

//this holds information for a single channel on a server
typedef struct channel_info channel_info;
struct channel_info {
	//logging information
	FILE *log_file;
	
	//name of this channel
	char name[BUFFER_SIZE];
	
	//text in this channel
	char *content[MAX_SCROLLBACK];
	
	//users in this channel (data in nick_info structs)
	dlist_entry *user_names;
	
	//store user count separately so we don't have to count it by iterating for non-nulls in the user_names array
	unsigned int nick_count;
	
	//topic for this channel
	char topic[BUFFER_SIZE];
	
	//client-defined state of this channel
	char was_pingged;
	char new_content;
	char is_pm;
};

//this holds an entire server's worth of information
typedef struct irc_connection irc_connection;
struct irc_connection {
	//behind-the-scenes data, the user never sees it
	int socket_fd;
	//a buffer for everything that's read from a socket
	//this carries over unterminated lines from the previous read
	//2* the normal buffer size since it may have to store two read buffer's worth of data concatenated together
	char parse_queue[2*BUFFER_SIZE];
	//the buffer to store what we're going to parse
	char read_buffer[BUFFER_SIZE];
	//timestamp of the last message received from this server
	time_t last_msg_time;
	//this data is stored in case the connection dies and we need to re-connect
	int port;
	//logging information
	char keep_logs;
	//what message to wait for before sending subsequent commands
	char post_type[BUFFER_SIZE];
	//everything to parse after a given server message is received
	dlist_entry *post_commands;
	
	//a dlist of lines (commands, probably) that get run on connection
	//for rc files and reconnects
	dlist_entry *server_run_lines;
	
	//whether, on this server, to rejoin channels when kicked
	char rejoin_on_kick;
	//the nick to use if the user's nick is already in use
	char fallback_nick[BUFFER_SIZE];
	//whether to try to reconnect to this server if connection is lost
	char reconnect;
	//whether to consider all pm messages (even after the first one) pings or not
	//if FALSE, only the first private message in a conversation is considered a ping, and others are not
	char ping_on_pms;
	//whether or not to hide joins/quits/parts in channel output on this server
	char hide_joins_quits;
	
#ifdef _OPENSSL
	//SSL additions
	//whether to use ssl for this connection or not (since read/write is different depending)
	char use_ssl;
	SSL *ssl_handle;
	SSL_CTX *ssl_context;

	//the ssl cert check status when this connection was first made
	//so that whatever setting was in-use at that time gets re-used on every reconnection attempt
	char ssl_cert_check;
#endif
	
	//this data is what the user sees (but is also used for other things)
	char server_name[BUFFER_SIZE];
	char nick[BUFFER_SIZE];
	
	//channel info for each channel we're on
	dlist_entry *ch;
	
	int current_channel;
	//the last user to PM us (so we know who to send a reply to)
	char last_pm_user[BUFFER_SIZE];
	
	//whether or not to display mode strings on PMs
	char use_mode_str;
	
	//quit message to use when no argument is given
	char quit_msg[BUFFER_SIZE];
	
	//an option for hidden_user_mode_changes
	//where you can specify a string, each character of which is a user mode whose changes should be hidden from the log
	//and setting the empty string should re-set behaviour to cause no user mode changes to be hidden
	char hidden_user_mode_changes[BUFFER_SIZE];
	
	//the order channels should be in post-join
	//default null, meaning view channels in the order they were joined
	dlist_entry *channel_order;
};

//this holds an "alias"
typedef struct alias alias;
struct alias {
	//what the user types to activate this alias
	char trigger[BUFFER_SIZE];
	//what is substituted for the alias before literal interpretation
	char substitution[BUFFER_SIZE];
};

//global variables

//whether or not to ignore the rc file in ~/.config
char ignore_rc;

//easy mode; set by default, turned off with the --proper cli switch
//when enabled, sets default aliases for nick, quit, and msg; also sets time format and auto-sends user quartet
char easy_mode;

//whether to center the server name (responsive behavior always)
char center_server_list;

#ifdef _OPENSSL

//enables the checking of SSL certificates prior to SSL connections
//enabled by default
char ssl_cert_check;

#endif

//whether or not to automatically create a faux-pm channel with /hi when PM'd
char auto_hi;

//the file pointer to log non-fatal errors to
FILE *error_file;

//whether we are able to make log files or not (TRUE if we can log)
char can_log;

//characters to escape commands for the server and client, respectively
char server_escape=DEFAULT_SERVER_ESCAPE;
char client_escape=DEFAULT_CLIENT_ESCAPE;

int current_server;
dlist_entry *servers;
//a history of lines the user has input so you can up and down arrow through them
char *input_history[MAX_SCROLLBACK];
//the location in input_history we're at now
int input_line;
//the location in the scrollback for the current channel we're at now
//(this var stores the index of the LAST line to output, hence "end")
int scrollback_end;
//where the previous scrollback was, so we know if it changed
int prev_scrollback_end;
//any aliases the user has registered, stored in a doubly-linked list
dlist_entry *alias_list;
//ping phrases
dlist_entry *ping_phrases;
//format to output time in (used for scrollback_output and the clock)
char time_format[BUFFER_SIZE];
//a custom string for CTCP VERSION responses (leave blank for default, we'll just check strlen is 0)
char custom_version[BUFFER_SIZE];
//whether or not we're currently listening for post commands
char post_listen;

//determine if we're done
char done;

//global width and height for the whole program (in characters)
//because there should never be inconsistency for that
int width=80;
int height=24;

WINDOW *server_list;
WINDOW *channel_list;
WINDOW *channel_topic;
WINDOW *channel_text;
WINDOW *user_input;
WINDOW *top_border;
WINDOW *bottom_border;

WINDOW *ncurses_fullscreen_text;

//forward declarations (trying to be minimal with these)
void scrollback_output(int server_index, int output_channel, char *to_output, char refresh);
void parse_input(char *input_buffer, char keep_history);
void refresh_server_list();
void refresh_channel_list();
void refresh_channel_topic();
void refresh_channel_text();
irc_connection *get_server(int server_index);
int get_current_channel(int server_index);
void privmsg_command(char *input_buffer);

//helper functions for common tasks

//find the first instance of needle in haystack
//if needle is not in haystack, return -1
//otherwise returns the index in haystack where the first instance of needle was found
//(get it? needle? haystack?, nvm screw you)
int strfind(char *needle, char *haystack){
	int haystack_index=0;
	//while we're not at the end of the haystack string
	while(haystack[haystack_index]!='\0'){
		int needle_index=0;
		//while each character matches and we're not at the end of the needle string
		while((needle[needle_index]==haystack[haystack_index+needle_index])&&(needle[needle_index]!='\0')){
			//go to the next char in the needle
			needle_index++;
		}
		//if we got to the end of the needle and everything matched
		if(needle[needle_index]=='\0'){
			//we found a match! return the index where that match started!
			return haystack_index;
		}
		
		//check the substring starting at the next char in the haystack
		haystack_index++;
	}
	//got to the end and didn't find a match
	return -1;
}

//store the substring of source (starting at start and going to length) in dest
//NOTE THIS IS UNSAFE, IT ASSUMES THE SIZE OF dest TO BE >= THE SIZE OF source
void substr(char *dest, char *source, unsigned int start, unsigned int length){
	int dest_index=0;
	//while we're in the substring we're supposed to be in and we haven't hit the end of the source
	while(((start+dest_index)<(start+length))&&(source[start+dest_index]!='\0')){
		dest[dest_index]=source[start+dest_index];
		dest_index++;
	}
	dest[dest_index]='\0';
}

//insert c into text at index, in a /relatively/ efficient (O(n)) manner
//for error-handling we also take in text_size which is the size of the buffer/char array in bytes
//returns TRUE on success, FALSE on failure
char strinsert(char *text, char c, int index, int text_size){
	int length=strlen(text);
	//if we can fit another character in the string
	//the -1 is for a NULL byte, since these are C strings
	if(length<text_size-1){
		//if we're inserting off the end it's an append
		if(index>=length){
			index=length;
		}
		
		//juggle and shift everything after index one position right without losing or corrupting anything
		char temp[2];
		//temp[1] is the character that will go into the postion we're currently considering
		temp[1]=c;
		//up to and including the null index
		while(index<=length){
			//temp[0] is the character that was previously at the postion we're currently considering
			temp[0]=text[index];
			//assign temp[1] to this position as expected
			text[index]=temp[1];
			//temp[0] will go into the next position
			temp[1]=temp[0];
			//move on to the next character
			index++;
		}
		//null-terminate (note this is equivalent to text[index]=temp[1], but it should always be NULL)
		text[index]='\0';
		
		return TRUE;
	}
	return FALSE;
}

//remove the char at index from text, in a /relatively/ efficient (O(n)) manner
//returns TRUE on success, FALSE on failure
char strremove(char *text, int index){
	int length=strlen(text);
	
	//if there are any characters to remove
	if(strlen(text)>0){
		//if they're asking to remove from past the end just remove from the end
		if(index>length){
			index=length-1;
		//and likewise on the start
		}else if(index<0){
			index=0;
		}
		
		//juggle and shift everything after index one positon left
		//note that on the null termination it will get shifted also
		while(index<length){
			text[index]=text[index+1];
			index++;
		}
		
		return TRUE;
	}
	return FALSE;
}

//convert an entire string to lower case, don't go over size (becase we don't want overflows)
//returns TRUE on success, FALSE on failure
char strtolower(char *text, int size){
	int n;
	for(n=0;n<size;n++){
		if(text[n]!='\0'){
			text[n]=tolower(text[n]);
		}else{
			return TRUE;
		}
	}
	return FALSE;
}

//reverse a string
void strnrev(char *text){
	int size=strlen(text);
	
	//go up to half of the size, swap the index and (size-1)-index elements
	int index;
	for(index=0;index<(size/2);index++){
		char tmp;
		tmp=text[index];
		text[index]=text[size-1-index];
		text[size-1-index]=tmp;
	}
	
	//defensive: null terminate no matter what
	text[size]='\0';
}

//put a morse equivalent of the given string in the given buffer
void morse_encode(char *text, char *out_buf, int output_length) {
	//a single ascii character can be up to 6 characters when encoded in morse
	//(i.e. a number and a trailing space)
	//so make sure we have enough room in the output, or else truncate the input
	if((strlen(text)*sizeof(char))>(output_length/6)){
		if(current_server>=0){
			scrollback_output(current_server,get_current_channel(current_server),"accirc: Warn: string too long to be reliably morse-encoded; truncating...",TRUE);
		}
		text[output_length/6]='\0';
	}
	int text_idx;
	int out_idx=0;
	for(text_idx=0;text_idx<strlen(text);text_idx++){
		//the character that we might convert to morse code
		//lower-case because morse code is case-insensitive
		char conv_char=tolower(text[text_idx]);
		char append_str[BUFFER_SIZE];
		snprintf(append_str,BUFFER_SIZE,"%c",conv_char);
		if((conv_char>='a') && (conv_char<='z')){
			snprintf(append_str,BUFFER_SIZE,"%s",morse_chars[conv_char-'a']);
		}else if((conv_char>='0') && (conv_char<='9')){
			snprintf(append_str,BUFFER_SIZE,"%s",morse_nums[conv_char-'0']);
		//don't convert dots and dashes; just put a space there
		}else if((conv_char=='.') || (conv_char=='-') || (conv_char==' ')){
			out_buf[out_idx]=' ';
			out_idx++;
			continue;
		}
		
		int append_idx;
		for(append_idx=0;append_idx<strlen(append_str);append_idx++){
			out_buf[out_idx]=append_str[append_idx];
			out_idx++;
		}
		//always put a trailing space between characters
		out_buf[out_idx]=' ';
		out_idx++;
		
		//as long as words are space-delimited when they come in
		//they will still be space-delimited when they come out
	}
	//always null terminate
	out_buf[out_idx]='\0';
}

//take a morse string and put an equivalent ascii string in the given buffer
void morse_decode(char *text, char *out_buf) {
	char conv_str[BUFFER_SIZE];
	
	//morse characters are space-delimited so to find a character look for a space
	int space_idx=strfind(" ",text);
	int last_space_idx=0;
	int out_buf_idx=0;
	char done=FALSE;
	while(!done){
		//if we found a space then check everything up to that against the known morse characters
		if(space_idx>=0){
			substr(conv_str,text,last_space_idx,space_idx-last_space_idx);
		//otherwise we found no space so check to end of string
		}else{
			strncpy(conv_str,text+(last_space_idx*sizeof(text[0])),BUFFER_SIZE);
			space_idx=strlen(text);
			done=TRUE;
		}
		if(strlen(conv_str)>0){
			//substitute for letters
			int char_idx;
			for(char_idx=0;char_idx<(sizeof(morse_chars)/sizeof(morse_chars[0]));char_idx++){
				if(strncmp(conv_str,morse_chars[char_idx],BUFFER_SIZE)==0){
					out_buf[out_buf_idx]=char_idx+'a';
					out_buf_idx++;
					break;
				}
			}
			//if no letters matched
			if(char_idx==(sizeof(morse_chars)/sizeof(morse_chars[0]))){
				//substitute for numbers
				for(char_idx=0;char_idx<(sizeof(morse_nums)/sizeof(morse_nums[0]));char_idx++){
					if(strncmp(conv_str,morse_nums[char_idx],BUFFER_SIZE)==0){
						out_buf[out_buf_idx]=char_idx+'0';
						out_buf_idx++;
						break;
					}
				}
				//otherwise nothing matched so just pass the character through as-is
				if(char_idx==(sizeof(morse_nums)/sizeof(morse_nums[0]))){
					int text_idx=last_space_idx;
					while(((space_idx>=0) && (text_idx<space_idx)) || ((space_idx<0) && (text_idx<strlen(text)))){
						out_buf[out_buf_idx]=text[text_idx];
						out_buf_idx++;
						text_idx++;
					}
				}
			}
		//two spaces of input map to one space of output because characters are expected to be space delimited
		//so words are two-space delimited
		}else if((space_idx>=0) && (space_idx==last_space_idx)) {
			out_buf[out_buf_idx]=' ';
			out_buf_idx++;
		}
		
		//look for the next space
		int text_idx=space_idx+1;
		last_space_idx=space_idx+1;
		if(space_idx>=0){
			while((text_idx<strlen(text)) && (text[text_idx]!=' ')){
				text_idx++;
			}
			if(text_idx==strlen(text)){
				text_idx=-1;
			}
			space_idx=text_idx;
		}
	}
	//always null-terminate
	out_buf[out_buf_idx]='\0';
}

//verify that a directory exists, if it doesn't, try to make it
//returns TRUE on succes, FALSE on failure
char verify_or_make_dir(char *path){
	//if making the directory failed for some reason other than it already existing
	if((mkdir(path,0777)==-1)&&(errno!=EEXIST)){
		//we failed
		return FALSE;
	}
	//if we got here and didn't return we succeeded
	return TRUE;
}

//verify or make all parent directories for a given file
char verify_or_make_parent_dirs_for(char *file_path){
	char path_prefix[BUFFER_SIZE];
	strncpy(path_prefix,"",BUFFER_SIZE);
	
	char partial_file_path[BUFFER_SIZE];
	strncpy(partial_file_path,file_path,BUFFER_SIZE);
	int first_slash=strfind("/",partial_file_path);
	//while this is a directory/path definition and not just a file
	while(first_slash>=0){
		//get the topmost directory to create
		char dir_name[BUFFER_SIZE];
		substr(dir_name,partial_file_path,0,first_slash);
		
		//along with its prefix (for everything up to this point)
		char dir_path[BUFFER_SIZE];
		snprintf(dir_path,BUFFER_SIZE,"%s/%s",path_prefix,dir_name);
		
		//and try to create it
		if(!verify_or_make_dir(dir_path)){
			return FALSE;
		}
		
		//and then update buffers to try to create the next subdirectory
		strncpy(path_prefix,dir_path,BUFFER_SIZE);
		char new_file_path[BUFFER_SIZE];
		//NOTE: the +1 here is because we skip over the slash character itself
		substr(new_file_path,partial_file_path,first_slash+1,strlen(partial_file_path)-(first_slash+1));
		strncpy(partial_file_path,new_file_path,BUFFER_SIZE);
		
		first_slash=strfind("/",partial_file_path);
	}
	
	//if we got here and didn't return we succeeded
	return TRUE;
}

irc_connection *get_server(int server_index){
	if((server_index<0)||(server_index>dlist_length(servers))){
		return NULL;
	}
	dlist_entry *server_entry=dlist_get_entry(servers,server_index);
	return (irc_connection *)(server_entry->data);
}

int get_current_channel(int server_index){
	irc_connection *server=get_server(server_index);
	if(server!=NULL){
		return server->current_channel;
	}
	return -1;
}

//find the index of a given nick on the given channel
//returns -1 if the nick is not found
int nick_idx(channel_info *ch, const char *nick, int start_idx){
	//can't search in NULL
	if((ch==NULL) || (nick==NULL)){
		return -1;
	}
	
	char lower_nick[BUFFER_SIZE];
	strncpy(lower_nick,nick,BUFFER_SIZE);
	strtolower(lower_nick,BUFFER_SIZE);
	
	int idx=start_idx;
	dlist_entry *nick_entry=dlist_get_entry(ch->user_names,start_idx);
	while(nick_entry!=NULL){
		nick_info *nick_content=(nick_info*)(nick_entry->data);
		char matching_name[BUFFER_SIZE];
		strncpy(matching_name,nick_content->user_name,BUFFER_SIZE);
		strtolower(matching_name,BUFFER_SIZE);
		if(strncmp(matching_name,lower_nick,BUFFER_SIZE)==0){
			return idx;
		}
		
		nick_entry=nick_entry->next;
		idx++;
	}
	return -1;
}

//find the index of the given channel in the given server's channel list
//NOTE: this CANNOT easliy be combined with the below ch_order_from_name function
//because they operate on different data types
//(server->ch is a list of channel_info pointers, server->channel_order is a list of strings)
int ch_idx_from_name(irc_connection *server, const char *ch_name){
	//NOTE: some servers give channel names with leading : characters in some contexts
	//such as for PART messages
	//as far as I know this isn't compliant with the spec and shouldn't be done
	//but nonetheless for compatibility we should be able to handle it
	int offset=0;
	if((strnlen(ch_name,BUFFER_SIZE)>0) && (ch_name[0]==':')){
		offset=1;
	}
	
	//channel names are case-insensitive
	//so before we do anything else, make a lower-case copy
	char channel[BUFFER_SIZE];
	strncpy(channel,ch_name+offset,BUFFER_SIZE-offset);
	strtolower(channel,BUFFER_SIZE);
	
	//go through the channels, searching for the given channel name
	int channel_index=0;
	dlist_entry *ch_entry=server->ch;
	while(ch_entry!=NULL){
		//note that we start at 1, 0 is always the reserved server channel
		if(channel_index==0){
			ch_entry=ch_entry->next;
			channel_index++;
			continue;
		}
		channel_info *ch=(channel_info *)(ch_entry->data);
		
		char lower_case_channel[BUFFER_SIZE];
		strncpy(lower_case_channel,ch->name,BUFFER_SIZE);
		strtolower(lower_case_channel,BUFFER_SIZE);
		
		//if we found the channel (case-insensitive)
		if(!strncmp(channel,lower_case_channel,BUFFER_SIZE)){
			//then return the index of that channel in the list
			return channel_index;
		}
		
		//we haven't found it yet, so go to the next node and keep trying
		ch_entry=ch_entry->next;
		channel_index++;
	}
	
	//if the channel wasn't found then signal that by returning a -1 to the calling code
	return -1;
}

//find the intended (not current) order of the given channel in the given server's channel_order list
//NOTE: this CANNOT easliy be combined with the above ch_idx_from_name function
//because they operate on different data types
//(server->ch is a list of channel_info pointers, server->channel_order is a list of strings)
int ch_order_from_name(irc_connection *server, const char *ch_name){
	//NOTE: some servers give channel names with leading : characters in some contexts
	//such as for PART messages
	//as far as I know this isn't compliant with the spec and shouldn't be done
	//but nonetheless for compatibility we should be able to handle it
	int offset=0;
	if((strnlen(ch_name,BUFFER_SIZE)>0) && (ch_name[0]==':')){
		offset=1;
	}
	
	//channel names are case-insensitive
	//so before we do anything else, make a lower-case copy
	char channel[BUFFER_SIZE];
	strncpy(channel,ch_name+offset,BUFFER_SIZE-offset);
	strtolower(channel,BUFFER_SIZE);
	
	//go through the channels, searching for the given channel name
	int ch_order_idx=0;
	dlist_entry *ch_order_entry=server->channel_order;
	while(ch_order_entry!=NULL){
		//NOTE: while for server->ch we start at index 1
		//for the ordering list we start at index 0
		//this is a difference between ch_order_from_name and ch_idx_from_name
		
		char lower_case_channel[BUFFER_SIZE];
		strncpy(lower_case_channel,(const char *)(ch_order_entry->data),BUFFER_SIZE);
		strtolower(lower_case_channel,BUFFER_SIZE);
		
		//if we found the channel (case-insensitive)
		if(!strncmp(channel,lower_case_channel,BUFFER_SIZE)){
			//then return the index of that channel in the list
			return ch_order_idx;
		}
		
		//we haven't found it yet, so go to the next node and keep trying
		ch_order_entry=ch_order_entry->next;
		ch_order_idx++;
	}
	
	//if the channel wasn't found then signal that by returning a -1 to the calling code
	return -1;
}

//determine the total length of characters in the server list
int calc_server_list_chars(){
	//if no server is available then return the character count for the error message
	//as that is what will be displayed
	if((servers==NULL)||(current_server<0)){
		return strlen("(no servers)");
	}
	
	int chr_count=0;
	dlist_entry *server_entry=servers;
	irc_connection *server=NULL;
	
	//for each server in the list
	while(server_entry!=NULL){
		server=(irc_connection *)(server_entry->data);
		
#ifdef _OPENSSL
		//a + is displayed in front of ssl-using server names
		if(server->use_ssl){
			chr_count+=strlen("+");
		}
#endif
		//the server name is the primary content
		//along with an associated delimiter string
		chr_count+=strlen(server->server_name);

		//if the nick isn't empty, we will be ouptutting that as well, in parens
		if(strncmp(server->nick,"",BUFFER_SIZE)!=0){
			chr_count+=strlen(server->nick)+strlen(" ()");
		}
		
		//include length for delimiters
		if(server_entry->next!=NULL){
			chr_count+=strlen(" | ");
		}
		
		server_entry=server_entry->next;
	}
	return chr_count;
}

//determine the total length of characters in the channel list
int calc_channel_list_chars(irc_connection *server){
	//if no server is available then return the character count for the error message
	//as that is what will be displayed
	if(server==NULL){
		return strlen("(no channels)");
	}
	
	int chr_count=0;
	dlist_entry *ch_entry=server->ch;
	channel_info *ch=NULL;
	
	//for each channel in the list
	//NOTE: the reserved "SERVER" entry is output the same way as any other channel
	while(ch_entry!=NULL){
		ch=(channel_info *)(ch_entry->data);
		
		//the channel name is the primary content
		//along with an associated delimiter string
		chr_count+=strlen(ch->name);
		
		if(ch_entry->next!=NULL){
			chr_count+=strlen(" | ");
		}
		
		ch_entry=ch_entry->next;
	}
	return chr_count;
}


//get information about whether the server passed in was pingged and/or has new (unread) content
void get_server_ping_state(irc_connection *server,char *was_pingged, char *new_server_content){
	*was_pingged=FALSE;
	*new_server_content=FALSE;
	
	//set the new server content to be the OR of all channels on that server
	dlist_entry *ch_entry=server->ch;
	while(ch_entry!=NULL){
		channel_info *ch=(channel_info *)(ch_entry->data);
		*new_server_content=((*new_server_content)||(ch->new_content));
		*was_pingged=((*was_pingged)||(ch->was_pingged));
		
		ch_entry=ch_entry->next;
	}
	//return nothing because new_server_content and was_pingged were set already by reference
}

//log an error to the global error_log file
//and include a timestamp when that error occurred
void error_log(const char *error_buffer){
	fprintf(error_file,"[%ju] %s\n",(uintmax_t)(time(NULL)),error_buffer);
}

//log a "ping" to a separate file
//line starts with given ping_time (usually PING or PM), and is server-local, but not channel-local (although channel is logged)
void ping_log(int server_index, const char *ping_type, const char *nick, const char *channel, const char *text){
	irc_connection *server=get_server(server_index);
	
	//if we were keeping logs then (try to) store this PM in the ping.txt file
	if((can_log) && (server->keep_logs)){
		char ping_file_buffer[BUFFER_SIZE];
		snprintf(ping_file_buffer,BUFFER_SIZE,"%s/.local/share/accirc/%s/pings.txt",getenv("HOME"),LOGGING_DIRECTORY);
		
		//try to make a ping log file, if that's impossible just give up
		FILE *ping_file=fopen(ping_file_buffer,"a");
		if(ping_file==NULL){
			error_log("Err: Could not find or create ping log file");
		}else{
			fprintf(ping_file,"(%s %s) %s: %ju <%s> %s\n",server->server_name,channel,ping_type,(uintmax_t)(time(NULL)),nick,text);
			fclose(ping_file);
		}
	}
}

//load an rc file as a dlist of server (irc_connection) objects
//NOTE: these are not TRUE irc connections because they aren't really connected
//and only serve as a placeholder for hostname, port, use_ssl, and server_run_lines
dlist_entry *load_rc_servers(char *rc_file){
	dlist_entry *rc_servers=NULL;
	int rc_server_index=-1;
	
	FILE *rc=fopen(rc_file,"r");
	if(!rc){
		error_log("Warn: rc file not found, not executing anything on startup");
		return NULL;
	}else{
		//read in the .rc, parse_input each line until the end
		char rc_line[BUFFER_SIZE];
		while(!feof(rc)){
			fgets(rc_line,BUFFER_SIZE,rc);
			
			//cut off the trailing newline
			int newline_index=strfind("\n",rc_line);
			if(newline_index>=0){
				substr(rc_line,rc_line,0,newline_index);
			}
			
			char is_server_run_line=FALSE;
			
			//client command
			if((strnlen(rc_line,BUFFER_SIZE)>0) && (rc_line[0]==client_escape)){
				//trim off the first character, which is the client escape character
				char tmp_buffer[BUFFER_SIZE];
				substr(tmp_buffer,rc_line,1,strlen(rc_line)-1);
				
				char command[BUFFER_SIZE];
				char parameters[BUFFER_SIZE];
				int first_space=strfind(" ",tmp_buffer);
				
				//space not found, command with no parameters
				if(first_space<0){
					strncpy(command,tmp_buffer,BUFFER_SIZE);
					strncpy(parameters,"",BUFFER_SIZE);
				}else{
					substr(command,tmp_buffer,0,first_space);
					substr(parameters,tmp_buffer,first_space+1,strlen(tmp_buffer)-(first_space+1));
				}
				
				//an rc file command to connect to a server, with or without encryption
				if((!strncmp("connect",command,BUFFER_SIZE)) || (!strncmp("sconnect",command,BUFFER_SIZE))){
					//make a new server structure to store information about this server
					irc_connection *rc_server=(irc_connection*)(malloc(sizeof(irc_connection)));
					
					//zero out the server structure to ensure no uninitialized data
					memset(rc_server,0,sizeof(irc_connection));
					
#ifdef _OPENSSL
					//save encryption option
					rc_server->use_ssl=FALSE;
					if(!strncmp("sconnect",command,BUFFER_SIZE)){
						rc_server->use_ssl=TRUE;
					}
#endif
					
					//store server host and port
					char host_param[BUFFER_SIZE];
					char port_param[BUFFER_SIZE];
					first_space=strfind(" ",parameters);
					if(first_space<0){
						strncpy(host_param,parameters,BUFFER_SIZE);
						strncpy(port_param,"6667",BUFFER_SIZE);
					}else{
						substr(host_param,parameters,0,first_space);
						substr(port_param,parameters,first_space+1,strlen(parameters)-(first_space+1));
					}
					
					strncpy(rc_server->server_name,host_param,BUFFER_SIZE);
					rc_server->port=atoi(port_param);
					
					//add this server to the doubly-linked list of rc servers
					//and update the index to reflect the new entry
					rc_server_index=dlist_length(rc_servers);
					rc_servers=dlist_append(rc_servers,rc_server);
					
				//TODO: ignore anything between /post and /no_post commands
				//as those are already handled by the server post_commands structure
				//and adding them to the server_run_lines as well would be redundant
				
				//any other client commands get added to the server's server_run_lines structure
				}else{
					//if this is a server escape (ser_escape) or client escape (cli_escape) command then run it right away
					//because otherwise we might be in the wrong state to parse the rest of the rc file
					if((!strncmp(command,"cli_escape",BUFFER_SIZE)) || (!strncmp(command,"ser_escape",BUFFER_SIZE))){
						parse_input(rc_line,FALSE);
					}else{
						is_server_run_line=TRUE;
					}
				}
			//any other lines before the next server get added to the server's server_run_lines structure
			}else{
				is_server_run_line=TRUE;
			}
			
			//if this is a non-empty line that should be added to server_run_lines
			//then add it now
			if((strnlen(rc_line,BUFFER_SIZE)>0) && (rc_server_index>=0) && is_server_run_line){
				char *server_run_line=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
				strncpy(server_run_line,rc_line,BUFFER_SIZE);
				irc_connection *rc_server=((irc_connection*)(dlist_get_entry(rc_servers,rc_server_index)->data));
				rc_server->server_run_lines=dlist_append(rc_server->server_run_lines,server_run_line);
			//if this would be a server_run_line except for the fact that we have no associated server
			}else if(strnlen(rc_line,BUFFER_SIZE)>0 && is_server_run_line){
				//then run it IMMEDIATELY, during this function call
				//as it is meant to apply prior to any server connection commands
				parse_input(rc_line,FALSE);
			}
		}
		fclose(rc);
	}
	return rc_servers;
}

//load the configuration file at path, returns TRUE on success, FALSE on failure
char load_rc(char *rc_file){
	dlist_entry *rc_servers=load_rc_servers(rc_file);
	
	//for each server we're meant to connect to
	for(int rc_server_index=0;rc_server_index<dlist_length(rc_servers);rc_server_index++){
		int connection_count=dlist_length(servers);
		
		irc_connection *rc_server=(irc_connection*)(dlist_get_entry(rc_servers,rc_server_index)->data);
		
		//create the appropriate connect command
		char input_buffer[BUFFER_SIZE];
		snprintf(input_buffer,BUFFER_SIZE,"/connect %s %i",rc_server->server_name,rc_server->port);
#ifdef _OPENSSL
		if(rc_server->use_ssl){
			snprintf(input_buffer,BUFFER_SIZE,"/sconnect %s %i",rc_server->server_name,rc_server->port);
		}
#endif
		
		//run the connect command
		//(yes, even though this is called parse_input it does in fact run the command, not just parse it)
		parse_input(input_buffer,FALSE);
		
		//store the value of current_server before we start running server_run_lines
		int prev_current_server=current_server;
		
		//if a connection actually was created
		if(dlist_length(servers)>connection_count){
			irc_connection *last_server=(irc_connection*)(dlist_get_entry(servers,dlist_length(servers)-1)->data);
			
			//and what we connected to was the server we expected to be connected to
			//(i.e. the thing our rc_server lines are for)
			if((!strncmp(last_server->server_name,rc_server->server_name,BUFFER_SIZE)) && (last_server->port==rc_server->port)){
#ifdef _OPENSSL
				if(last_server->use_ssl==rc_server->use_ssl){
#endif
					//set the current/active/viewed server to be the last server in the list
					//since we're about to run a bunch of commands
					//and we want to only run them in the context of that particular server connection
					current_server=dlist_length(servers)-1;
					
					//clear out any previously-existing server_run_lines
					//just to make sure we're ONLY including the lines specified in the rc file
					if(dlist_length(last_server->server_run_lines)>0){
						dlist_free(last_server->server_run_lines,TRUE);
						last_server->server_run_lines=NULL;
					}
					//copy the rc_server's server_run_lines to the actual connected server object
					last_server->server_run_lines=dlist_deep_copy(rc_server->server_run_lines,sizeof(char)*(BUFFER_SIZE+1));
					
					//for each server run line
					for(int server_run_line_index=0;server_run_line_index<dlist_length(last_server->server_run_lines);server_run_line_index++){
						strncpy(input_buffer,(const char*)(dlist_get_entry(last_server->server_run_lines,server_run_line_index)->data),BUFFER_SIZE);
						
						//actually run it now that we're connected
						parse_input(input_buffer,FALSE);
					}
					
#ifdef _OPENSSL
				}
#endif
			}
		}
		
		//reset the current_server to what it was before we started running server_run_lines
		//if it was any valid value to start with
		if(prev_current_server>=0){
			current_server=prev_current_server;
		}
		
		refresh_server_list();
		refresh_channel_list();
		refresh_channel_topic();
		refresh_channel_text();
	}
	
	//clean up memory from the config file servers
	//since we're now done reading the rc file
	//and everything that was successfully connected should be stored in the global servers list
	dlist_free(rc_servers,TRUE);
	
	post_listen=FALSE;
	return TRUE;
}

//writes over an entire window of given size with space characters
//I'm hoping this fixes the flicker issues, which I think are caused by wclear calls
void wblank(WINDOW *win, int win_width, int win_height){
//	wclear(win);
	
	//go through the entire area and write spaces over it
	int x;
	for(x=0;x<win_width;x++){
		int y;
		for(y=0;y<win_height;y++){
			//ncurses takes y first in wmove, don't judge
			wmove(win,y,x);
//			wprintw(win,"%c",' ');
			waddch(win,' ');
		}
	}
	
	//re-set to 0,0 when done
	wmove(win,0,0);
}

void wcoloron(WINDOW *win, int fg, int bg){
	wattron(win,COLOR_PAIR((fg<<4)|(bg<<0)));
}

void wcoloroff(WINDOW *win, int fg, int bg){
	wattroff(win,COLOR_PAIR((fg<<4)|(bg<<0)));
}

//attempts to connect to given host and port
//returns a descriptor for the socket used, -1 if connection was unsuccessful
char make_socket_connection(char *input_buffer, char *host, int port, const struct in_addr *own_ip){
	//a string buffer to hold error messages
	char error_buffer[BUFFER_SIZE];
	
	//the address of the server
	struct sockaddr_in serv_addr;
	
	//set the socket file descriptor
	//arguments mean ipv4, stream type, and tcp protocol
	int new_socket_fd=socket(AF_INET,SOCK_STREAM,0);
	
	if(new_socket_fd<0){
		//handle failed socket openings gracefully here (by telling the user and giving up)
		error_log("Err: Could not open socket");
		strncpy(input_buffer,"\0",BUFFER_SIZE);
		return -1;
	}
	
	//that's "get host by name"
	struct hostent *server=gethostbyname(host);
	if(server==NULL){
		//handle failed hostname lookups gracefully here (by telling the user and giving up)
		snprintf(error_buffer,BUFFER_SIZE,"Err: Could not find server \"%s\"",host);
		error_log(error_buffer);
		strncpy(input_buffer,"\0",BUFFER_SIZE);
		return -1;
	}
	
	//configure the server address information
	bzero((char *)(&serv_addr),sizeof(serv_addr));
	serv_addr.sin_family=AF_INET;
	bcopy((char *)(server->h_addr),(char *)(&serv_addr.sin_addr.s_addr),server->h_length);
	serv_addr.sin_port=htons(port);
	
	//include an optional own_ip setting that allows us to how our own hostname is shown
	//NOTE: for now we're only supporting ipv4
	//although I would love to add ipv6 support as soon as my ISP allows it...
	if(own_ip!=NULL){
#ifdef DEBUG
		char error_buffer[BUFFER_SIZE];
		snprintf(error_buffer,BUFFER_SIZE,"dbg: setting own_ip to %u with memcpy",(*((u_int32_t *)own_ip)));
		error_log(error_buffer);
#endif
		
		memcpy(&serv_addr.sin_addr,&own_ip,sizeof(struct in_addr));
	}
	
	//if we could successfully connect
	//(side effects happen during this call to connect())
	if(connect(new_socket_fd,(struct sockaddr *)(&serv_addr),sizeof(serv_addr))<0){
		//handle failed connections gracefully here (by telling the user and giving up)
		snprintf(error_buffer,BUFFER_SIZE,"Err: Could not connect to server \"%s\"",host);
		error_log(error_buffer);
		strncpy(input_buffer,"\0",BUFFER_SIZE);
		return -1;
	}
	
	//if we got here and didn't return all was well, return the new socket descriptor!
	return new_socket_fd;
}

//returns TRUE if successful
//FALSE on error
char server_write(int server_index, char *buffer){
	//properly terminate the buffer just in case
	buffer[BUFFER_SIZE-1]='\0';
	irc_connection *server=get_server(server_index);
#ifdef _OPENSSL
	if(server->use_ssl){
		if(SSL_write(server->ssl_handle,buffer,strlen(buffer))<0){
			error_log("Err: Could not send (ecrypted) data");
			return FALSE;
		}
	}else{
#endif
	if(write(server->socket_fd,buffer,strlen(buffer))<0){
		error_log("Err: Could not send data");
		return FALSE;
	}
#ifdef _OPENSSL
	}
#endif
	return TRUE;
}


//returns TRUE if successful
//FALSE on error
char server_read(int server_index, char *buffer){
	int bytes_transferred;
	irc_connection *server=get_server(server_index);
#ifdef _OPENSSL
	if(server->use_ssl){
		bytes_transferred=SSL_read(server->ssl_handle,buffer,BUFFER_SIZE-1);
	}else{
#endif
	bytes_transferred=read(server->socket_fd,buffer,BUFFER_SIZE-1);
#ifdef _OPENSSL
	}
#endif
	//if there's nothing to read right now just do nothing and check again later
	if(errno==EAGAIN){
		return FALSE;
	}
	
	if(bytes_transferred<=0){
		error_log("Err: Could not receive data");
		return FALSE;
	}
	//clear out the remainder of the buffer just in case
	int n;
	for(n=bytes_transferred;n<BUFFER_SIZE;n++){
		buffer[n]='\0';
	}
	return TRUE;
}

//formats the given time and stores the result in the given buffer
void custom_format_time(char *time_buffer, time_t current_unixtime){
	struct tm *current_localtime=localtime(&current_unixtime);
	strftime(time_buffer,BUFFER_SIZE,time_format,current_localtime);
	
	//NOTE: apparently current_localtime gets free'd by strftime or something; definitely don't do it here
}

//returns the character to show before a user's nick
//based on the list of modes which they currenty have applied to them
char get_mode_prefix(char *mode_str){
	char ret='\0';
	unsigned int idx;
	for(idx=0;idx<strlen(mode_str);idx++){
		//precedence rules: q (~) > o (@) > h (%) > v (+)
		switch(mode_str[idx]){
			case 'q': //channel owner
			case 'a': //channel owner (slashnet)
				ret='~';
				break;
			case 'o': //channel operator
				if(ret!='~'){
					ret='@';
				}
				break;
			case 'h': //channel half operator
				if((ret!='~') && (ret!='@')){
					ret='%';
				}
				break;
			case 'v': //voice
				if((ret!='~') && (ret!='@') && (ret!='%')){
					ret='+';
				}
				break;
/*
			case 'v': //TODO: find correct mode for &
				ret='&';
				break;
*/
		}
	}
	return ret;
}

//stores the given mode_prefix in the given mode_str
void set_mode_str_from_prefix(char *mode_str,const char *mode_prefix,unsigned int max_out_len){
	unsigned int out_idx=0;
	
	unsigned int idx;
	for(idx=0;idx<strlen(mode_prefix);idx++){
		char new_mode='\0';
		switch(mode_prefix[idx]){
			case '~':
				new_mode='q';
				break;
			case '@':
				new_mode='o';
				break;
			case '%':
				new_mode='h';
				break;
			case '+':
				new_mode='v';
				break;
/*
			case '&': //TODO: find correct mode for &
				new_mode='v';
				break;
*/
			//if we got an unrecognized mode character
			//then this is likely part of a nick
			//so stop looking for mode characters in that case
			default:
				idx=strlen(mode_prefix);
				break;
		}
		//if a mode was recognized and we haven't filled up the output buffer
		//then add this mode to the output buffer
		//NOTE: the +1 here is because we need to leave a character for the null termination
		if((new_mode!='\0') && ((out_idx+1)<max_out_len)){
			mode_str[out_idx]=new_mode;
			out_idx++;
		}
	}
	//null terminate strings
	mode_str[out_idx]='\0';
	
	//no return; mode_str changed as side-effect
}

//handles SIGHUP by sending a default exit message to every server currently in use
void terminal_close(int signal){
	char quit_buffer[BUFFER_SIZE];
	snprintf(quit_buffer,BUFFER_SIZE,"%cexit %s",client_escape,DEFAULT_TERM_LOST_MESSAGE);
	parse_input(quit_buffer,FALSE);
}

//TODO: refactor this, as this function is currently over 200 lines long!
//returns the new index of the server if a reconnection was made successfully
//returns -1 if close completed without reconnection
int properly_close(int server_index){
	//if we're not connected in the first place just leave, we're done here
	if((server_index<0) || (server_index>=dlist_length(servers))){
		return -1;
	}
	//remember where this server was in the list in case we reconnect
	//in which case we want to preserve the server order
	int reconnect_server_index=server_index;
	
	dlist_entry *server_entry=dlist_get_entry(servers,server_index);
	irc_connection *server=(irc_connection *)(server_entry->data);
	
	//if we want to reconnect to this server handle that
	char reconnect_this=server->reconnect;
	
	char reconnect_host[BUFFER_SIZE];
	int reconnect_port;
	char reconnect_nick[BUFFER_SIZE];
	char reconnect_fallback_nick[BUFFER_SIZE];
	char reconnect_post_type[BUFFER_SIZE];
	dlist_entry *reconnect_post_commands=NULL;
	dlist_entry *reconnect_server_run_lines=NULL;
	
#ifdef _OPENSSL
	//whether to use ssl when we reconnect
	char reconnect_with_ssl=FALSE;
	
	//and wheter or not to verify certs
	char reconnect_ssl_cert_check=FALSE;
#endif
	
	//if we'll be reconnecting to this server
	if(reconnect_this){
		//remember all the necessary information in order to reconnect
		strncpy(reconnect_host,server->server_name,BUFFER_SIZE);
		reconnect_port=server->port;
		
		//we'll automatically rejoin the channels we had on auto-join anyway, using the post command
		strncpy(reconnect_post_type,server->post_type,BUFFER_SIZE);
		reconnect_post_commands=dlist_deep_copy(server->post_commands,sizeof(char)*(BUFFER_SIZE+1));
		reconnect_server_run_lines=dlist_deep_copy(server->server_run_lines,sizeof(char)*(BUFFER_SIZE+1));
		
		strncpy(reconnect_nick,server->nick,BUFFER_SIZE);
		
		//if the user had a nick set, then use that on reconnect
		strncpy(reconnect_fallback_nick,server->nick,BUFFER_SIZE);
		if(!strcmp(reconnect_fallback_nick,"")){
			//if they didn't have a nick set, then use the fallback nick, if one was configured
			strncpy(reconnect_fallback_nick,server->fallback_nick,BUFFER_SIZE);
		}
		
#ifdef _OPENSSL
		reconnect_with_ssl=server->use_ssl;
		reconnect_ssl_cert_check=server->ssl_cert_check;
#endif
	}
	
#ifdef _OPENSSL
	if(server->use_ssl){
		if(server->ssl_handle!=NULL){
			SSL_free(server->ssl_handle);
		}
		if(server->ssl_context!=NULL){
			SSL_CTX_free(server->ssl_context);
		}
	}
#endif
	
	//clean up the server
	close(server->socket_fd);
	
	//free RAM null this sucker out
	//NOTE: because there are pointers in this data we can't do a naive dlist_free
	//because that wouldn't free the channel content (scrollback)
	dlist_entry *ch_entry=server->ch;
	while(ch_entry!=NULL){
		channel_info *ch=(channel_info *)(ch_entry->data);
		int n1;
		for(n1=0;n1<MAX_SCROLLBACK;n1++){
			if(ch->content[n1]!=NULL){
				free(ch->content[n1]);
			}
		}
		
		if(ch->log_file!=NULL){
			fclose(ch->log_file);
		}
		
		dlist_free(ch->user_names,TRUE);
		
		ch_entry=ch_entry->next;
	}
	dlist_free(server->ch,TRUE);
	dlist_free(server->channel_order,TRUE);
	
	dlist_free(server->post_commands,TRUE);
	dlist_free(server->server_run_lines,TRUE);
	
	//delete this item from the global list of servers
	servers=dlist_delete_entry(servers,server_index,TRUE);
	
	//if we were on a server after that one, then decrement current_server to stay on the same previously-active server
	//as the index will change due to the list getting shorter
	if(current_server>server_index){
		current_server--;
	//set a new current_server if we were on that one
	}else if(current_server==server_index){
		current_server=(dlist_length(servers)>0)?0:-1;
	}

	//as of now the index of the server is non-existant
	//and we will return as such unless a reconnection is successful in the interim
	server_index=-1;
	
	//if we'll be reconnecting to this server, and we didn't exit yet
	if(reconnect_this && (!done)){
		char reconnect_command[BUFFER_SIZE];
		snprintf(reconnect_command,BUFFER_SIZE,"%cconnect %s %i",client_escape,reconnect_host,reconnect_port);
		
#ifdef _OPENSSL
		if(reconnect_with_ssl){
			snprintf(reconnect_command,BUFFER_SIZE,"%csconnect %s %i",client_escape,reconnect_host,reconnect_port);
		//else keep the default reconnect_command that was set just above this
		}
#endif
		
		//find where the next connection will go so we know if the command was successful
		int next_server=dlist_length(servers);
		
		//try to reconnect again and again until we're either connected or we timed out
		char timeout=FALSE;
		time_t start_time=time(NULL);
		while((!timeout)&&(dlist_get_entry(servers,next_server)==NULL)){
			time_t current_time=time(NULL);
			if((current_time-start_time)>RECONNECT_TIMEOUT){
				timeout=TRUE;
			}else{
#ifdef _OPENSSL
				//save the current global ssl cert check setting into a temporary variable
				//and re-set it to the value that was used for the initial connection to this server
				char tmp_ssl_cert_check=ssl_cert_check;
				ssl_cert_check=reconnect_ssl_cert_check;
#endif
				//send the connect command (if this works then dlist_get_entry(servers,next_server) should no longer be NULL, breaking the loop)
				parse_input(reconnect_command,FALSE);
#ifdef _OPENSSL
				//restore global ssl cert check setting
				ssl_cert_check=tmp_ssl_cert_check;
#endif
			}
		}
		
		//because there wasn't a timeout, implicitly we successfully connected
		if(!timeout){
			//NOTE: we know that server_entry will NOT be null because the above while loop exited and we know timeout is false
			//therefore dlist_get_entry(servers,next_server) returned a non-null value prior to this code being executed
			server_entry=dlist_get_entry(servers,next_server);
			server=(irc_connection *)(server_entry->data);

			int old_server;
			if(current_server>=0){
				old_server=current_server;
			}else{
				old_server=next_server;
			}
			
			//we're connected, so time to do some stuff
			current_server=next_server;
			
			//the last message timestamp is now the current time
			server->last_msg_time=time(NULL);
			
			char command_buffer[BUFFER_SIZE];
			snprintf(command_buffer,BUFFER_SIZE,"%cnick %s",server_escape,reconnect_nick);
			parse_input(command_buffer,FALSE);
			//the syntax for USER is "USER <username> <hostname> <servername> <realname>"
			snprintf(command_buffer,BUFFER_SIZE,"%cuser %s %s %s %s",server_escape,reconnect_nick,reconnect_host,reconnect_host,reconnect_nick);
			parse_input(command_buffer,FALSE);
			
			snprintf(command_buffer,BUFFER_SIZE,"%cfallback_nick %s",client_escape,reconnect_fallback_nick);
			parse_input(command_buffer,FALSE);

			//TODO: fix the segfault that occasionally occurs somewhere around here
			//the last output in these cases is the fallback_nick set result
			//(I think this might have been implicity fixed when other changes were made but it needs to be tested thoroughly)
			
			//if you wanted to reconnect before you probably still want to
			//this segfaults with parse_input but doesn't fail at all with just setting the value, not sure why that is
//			parse_input("/reconnect",FALSE); //this should be client_escape instead of literal "/" anyway...
			server->reconnect=TRUE;
			
			//re-send all the auto-sent text when relevant, just like on first connection
			//note that the post_commands MUST start with the server escape or client escape,
			//because on bouncer connections join may happen before the post_type message,
			//and the commands might get leaked to a channel if they don't start with an escape character
			strncpy(server->post_type,reconnect_post_type,BUFFER_SIZE);
			
			//NOTE: this is now redundant with server_run_lines
			//so we are leaving server->post_commands in the default NULL state it was set to in add_server
			//and it should get auto-repopulated when the server_run_lines are executed later
//			server->post_commands=dlist_deep_copy(reconnect_post_commands,(sizeof(char)*(BUFFER_SIZE+1)));
			
			server->server_run_lines=dlist_deep_copy(reconnect_server_run_lines,sizeof(char)*(BUFFER_SIZE+1));
			
			//for each server run line
			for(int server_run_line_index=0;server_run_line_index<dlist_length(server->server_run_lines);server_run_line_index++){
				//actually run it now that we're connected
				strncpy(command_buffer,(const char*)(dlist_get_entry(server->server_run_lines,server_run_line_index)->data),BUFFER_SIZE);
				parse_input(command_buffer,FALSE);
			}
			
			//go back to whatever server you were on prior to the reconnection operation
			current_server=old_server;
			
			//set the new server index as the return value of this function
			server_index=next_server;
			
			//if the connection made was not at the same place as the previous connection
			//then perform an insert at that location
			if(next_server!=reconnect_server_index){
				//set the updated (post-move) server index as the return value of this function
				server_index=dlist_move_node(&servers,next_server,reconnect_server_index);
				if(old_server>reconnect_server_index){
					current_server=old_server-1;
				}
			}
		}
	}
	
	//free memory that was for a reconnect buffer
	//if we needed it then we made a deep copy already in the server structure
	dlist_free(reconnect_post_commands,TRUE);
	dlist_free(reconnect_server_run_lines,TRUE);
	
	//output
	if(current_server<0){
		wblank(server_list,width,1);
		wblank(channel_list,width,1);
		wblank(channel_topic,width,1);
		wblank(channel_text,width,height-RESERVED_LINES);
		
		wprintw(server_list,"(no servers)");
		wprintw(channel_list,"(no channels)");
		wprintw(channel_topic,"(no channel topic)");
		wprintw(channel_text,"(no channel text)");
		
		wrefresh(server_list);
		wrefresh(channel_list);
		wrefresh(channel_topic);
		wrefresh(channel_text);
	}else{
		refresh_server_list();
		refresh_channel_list();
		refresh_channel_topic();
		refresh_channel_text();
	}

	return server_index;
}

void wprintw_ping_effects(WINDOW *ncurses_win, const char *outstr, char was_pingged, char new_content){
	//if there was new data
	if(new_content==TRUE){
		//if there was also a ping, show bold AND underline
		if(was_pingged==TRUE){
			wattron(ncurses_win,A_BOLD);
		}
		
		wattron(ncurses_win,A_UNDERLINE);
		wprintw(ncurses_win,outstr);
		wattroff(ncurses_win,A_UNDERLINE);
		
		if(was_pingged==TRUE){
			wattroff(ncurses_win,A_BOLD);
		}
	//otherwise just display it regularly
	}else{
		wprintw(ncurses_win,outstr);
	}
}

//refresh the title for a SINGLE server
//NOTE: the calling code is expected to wmove() to where it wants this
//(within the server_list window)
//prior to calling this function
void refresh_server_title(irc_connection *server, char is_active, char add_delimiter){
	if(server==NULL){
		return;
	}

	//tells us if there is new content on this server since the user last viewed it
	char new_server_content=FALSE;
	char was_pingged=FALSE;
	get_server_ping_state(server,&was_pingged,&new_server_content);
	
#ifdef _OPENSSL
	//if this server is using encryption for our connection display that with a "+"
	if(server->use_ssl){
		wprintw(server_list,"+");
	}
#endif
	
	//if it's the active server bold it
	if(is_active==TRUE){
		wattron(server_list,A_BOLD);
		wprintw(server_list,server->server_name);
		wattroff(server_list,A_BOLD);
		
		//if we're viewing this server any content that would be considered "new" is no longer there
		new_server_content=FALSE;
	}else{
		//if there is new data on this server we're currently iterating on, display differently to show that to the user
		//otherwise just display it regularly
		wprintw_ping_effects(server_list,server->server_name,was_pingged,new_server_content);
	}
	
	//and display the nickname we're using for that server
	if(strcmp(server->nick,"")!=0){
		wprintw(server_list," (");
		wprintw(server_list,server->nick);
		wprintw(server_list,")");
	}
	
	if(add_delimiter==TRUE){
		//add a delimiter for formatting purposes
		wprintw(server_list," | ");
	}
}

//display server list updates as needed when we don't have room to display the full list
//bolding the current server
void refresh_server_list_centered(){
	//NOTE: we already checked that we were connected in refresh_server_list prior to this call
	//so we can assume that current_server>=0 and servers!=NULL at this point
	
	//update the display of the server list
	wblank(server_list,width,1);
	
	//start by outputting the selected server only
	//so move to where we want that
	irc_connection *server=get_server(current_server);
	int sel_x_coord=((width)-strlen(server->server_name)-strlen(server->nick)-strlen(" ()"))/2;
	//do not allow negatives; if we don't have room to output even one server then start at 0 and output what we can
	sel_x_coord=(sel_x_coord<0)?0:sel_x_coord;
	wmove(server_list,0,sel_x_coord);
	
	//and output just that
	refresh_server_title(server,TRUE,FALSE);

	//save ping and new data states for servers prior to this one (OR-d)
	//and servers after this one (OR-d)
	char was_pingged_prev=FALSE;
	char new_content_prev=FALSE;
	char was_pingged_next=FALSE;
	char new_content_next=FALSE;

	//iterate through servers to determine their ping/new data states
	int n=0;
	dlist_entry *server_entry=servers;
	
	//for each server in the list
	while(server_entry!=NULL){
		//NOTE: on the first iteration of this loop
		//this over-writes the variable that previously stored
		//the current server state
		server=(irc_connection *)(server_entry->data);
		
		char was_pingged=FALSE;
		char new_server_content=FALSE;
		get_server_ping_state(server,&was_pingged,&new_server_content);
		
		if(n<current_server){
			was_pingged_prev=((was_pingged_prev)||(was_pingged));
			new_content_prev=((new_content_prev)||(new_server_content));
		}else if(n==current_server){
			//we already did output for the current server so just skip this
		}else{ //n>current_server
			was_pingged_next=((was_pingged_next)||(was_pingged));
			new_content_next=((new_content_next)||(new_server_content));
		}

		server_entry=server_entry->next;
		n++;
	}

	//output arrows as needed
	
	//left arrow shows that other servers exist prior to this one
	if(current_server>0){
		wmove(server_list,0,0);
		wprintw_ping_effects(server_list,"<(F3)",was_pingged_prev,new_content_prev);
		wprintw(server_list," ");
	}
	//right arrow shows that other servers exist after this one
	if(current_server<(dlist_length(servers)-1)){
		wmove(server_list,0,width-strlen(" (F4)>"));
		wprintw(server_list," ");
		wprintw_ping_effects(server_list,"(F4)>",was_pingged_next,new_content_next);
	}
	
	wrefresh(server_list);
}


//display server list updates as needed; bolding the current server
void refresh_server_list(){
	//if we're not connected to anything don't bother
	if((servers==NULL)||(current_server<0)){
		return;
	}
	
	//if an ncurses window doesn't exist for this display
	//then we can't display anything
	if(server_list==NULL){
		return;
	}
	
	//if we can't display everything
	unsigned int server_list_chars=calc_server_list_chars();
	if(server_list_chars>width){
		//then center the current item and show arrows
		refresh_server_list_centered();
		
		//and cease and desist all further output (which would over-write that)
		return;
	}
	
	//calculate the offset from the left side to center the server list text
	//by calculating the total output width, then getting
	//lchar_offset = (screen_width - output_width)/2;
	unsigned int offset=(width-server_list_chars)/2;
	
	//update the display of the server list
	wblank(server_list,width,1);
//	wclear(server_list); //this shouldn't be needed and causes flicker!
	
	//if we're centering, apply offset
	if(center_server_list){
		wmove(server_list,0,offset);
	}else{
		wmove(server_list,0,0);
	}
	
	int n=0;
	dlist_entry *server_entry=servers;
	irc_connection *server=NULL;
	
	//for each server in the list
	while(server_entry!=NULL){
		server=(irc_connection *)(server_entry->data);
		
		refresh_server_title(server,(current_server==n)?TRUE:FALSE,(server_entry->next!=NULL));

		server_entry=server_entry->next;
		n++;
	}
	wrefresh(server_list);
}

//refresh the title for a SINGLE channel
//NOTE: the calling code is expected to wmove() to where it wants this
//(within the channel_list window)
//prior to calling this function
void refresh_channel_title(channel_info *ch, char is_active, char add_delimiter){
	//if it's the active channel then bold it
	if(is_active==TRUE){
		wattron(channel_list,A_BOLD);
		wprintw(channel_list,ch->name);
		wattroff(channel_list,A_BOLD);
		
		//if we're viewing this channel any content that would be considered "new" is no longer there
		ch->new_content=FALSE;
		
		//likewise a ping is now obsolete
		ch->was_pingged=FALSE;
	}else{
		wprintw_ping_effects(channel_list,ch->name,ch->was_pingged,ch->new_content);
	}
	
	if(add_delimiter==TRUE){
		//add a delimiter for formatting purposes
		wprintw(channel_list," | ");
	}
}

//display channel list updates as needed when we don't have room to display the full list
//bolding the current channel
//TODO: when outputting centered channel, attempt to output the channel immediately to the left and the channel immediately to the right, if there's room
void refresh_channel_list_centered(irc_connection *server){
	//NOTE: we already checked that we were connected in refresh_channel_list prior to this call
	//so we can assume that current_server>=0 and servers!=NULL at this point
	
	//update the display of the channel list
	wblank(channel_list,width,1);
	
	//start by outputting the selected channel only
	//so move to where we want that
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
	
	int sel_x_coord=((width)-strlen(ch->name))/2;
	//do not allow negatives; if we don't have room to output even one channel then start at 0 and output what we can
	sel_x_coord=(sel_x_coord<0)?0:sel_x_coord;
	wmove(channel_list,0,sel_x_coord);
	
	//and output just that
	refresh_channel_title(ch,TRUE,FALSE);

	//save ping and new data states for channels prior to this one (OR-d)
	//and channels after this one (OR-d)
	char was_pingged_prev=FALSE;
	char new_content_prev=FALSE;
	char was_pingged_next=FALSE;
	char new_content_next=FALSE;

	//iterate through channels to determine their ping/new data states
	int n=0;
	dlist_entry *ch_entry=server->ch;
	
	//for each channel in the list
	while(ch_entry!=NULL){
		//NOTE: on the first iteration of this loop
		//this over-writes the variable that previously stored
		//the current channel state
		ch=(channel_info *)(ch_entry->data);
		
		if(n<server->current_channel){
			was_pingged_prev=((was_pingged_prev)||(ch->was_pingged));
			new_content_prev=((new_content_prev)||(ch->new_content));
		}else if(n==server->current_channel){
			//we already did output for the current server so just skip this
		}else{ //n>server->current_channel
			was_pingged_next=((was_pingged_next)||(ch->was_pingged));
			new_content_next=((new_content_next)||(ch->new_content));
		}

		ch_entry=ch_entry->next;
		n++;
	}

	//output arrows as needed
	
	//left arrow shows that other channels exist prior to this one
	if(server->current_channel>0){
		wmove(channel_list,0,0);
		wprintw_ping_effects(channel_list,"<(F1)",was_pingged_prev,new_content_prev);
		wprintw(channel_list," ");
	}
	//right arrow shows that other channels exist after this one
	if(server->current_channel<(dlist_length(server->ch)-1)){
		wmove(channel_list,0,width-strlen(" (F2)>"));
		wprintw(channel_list," ");
		wprintw_ping_effects(channel_list,"(F2)>",was_pingged_next,new_content_next);
	}
	
	wrefresh(channel_list);
}

//display channel list updates as needed; bolding the current channel
void refresh_channel_list(){
	irc_connection *server=get_server(current_server);
	
	//if we're not connected to anything don't bother
	if(server==NULL){
		return;
	}
	
	//if an ncurses window doesn't exist for this display
	//then we can't display anything
	if(channel_list==NULL){
		return;
	}
	
	//if we can't display everything at once
	unsigned int channel_list_chars=calc_channel_list_chars(server);
	if(channel_list_chars>width){
		//then cneter the current channel and show arrows
		refresh_channel_list_centered(server);
		
		//and don't do anything else
		return;
	}

	//calculate the offset from the left side to center the server list text
	//by calculating the total output width, then getting
	//lchar_offset = (screen_width - output_width)/2;
	unsigned int offset=(width-channel_list_chars)/2;
	
	//update the display of the channel list
	wblank(channel_list,width,1);
//	wclear(channel_list); //this shouldn't be needed and causes flicker!
	
	//if we're centering, apply offset
	if(center_server_list){
		wmove(channel_list,0,offset);
	}else{
		wmove(channel_list,0,0);
	}
	
	dlist_entry *ch_entry=server->ch;
	int n=0;
	while(ch_entry!=NULL){
		channel_info *ch=(channel_info *)(ch_entry->data);
		
		refresh_channel_title(ch,(server->current_channel==n)?TRUE:FALSE,(ch_entry->next!=NULL));
			
		ch_entry=ch_entry->next;
		n++;
	}
	wrefresh(channel_list);
}

//display channel topic as needed 
void refresh_channel_topic(){
	irc_connection *server=get_server(current_server);
	
	//if we're not connected to anything don't bother
	if(server==NULL){
		return;
	}
	
	//if an ncurses window doesn't exist for this display
	//then we can't display anything
	if(channel_topic==NULL){
		return;
	}
	
	//get the relevant channel from the server's list of channels
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
	
	//print out the channel topic
	//first clearing that window
	wblank(channel_topic,width,1);
	
	//start at the start of the line
	wmove(channel_topic,0,0);
	char topic[BUFFER_SIZE];
	strncpy(topic,ch->topic,BUFFER_SIZE);
	if(strlen(topic)<width){
		wprintw(channel_topic,topic);
	}else{
		//NOTE: although we're not outputting the full line here, the full line WILL be in the logs for the user to view
		//and WILL be in the server information should the user resize the window
		int n;
		for(n=0;n<width;n++){
			topic[n]=ch->topic[n];
			//unicode and bold are ignored in the topic, and just displayed as ?
			if(topic[n]==0x02 || ((topic[n] & 128)>0)){
				topic[n]='?';
			}
		}
		char line_overflow_error[BUFFER_SIZE];
		strncpy(line_overflow_error,LINE_OVERFLOW_ERROR,BUFFER_SIZE);
		for(n=width-strlen(line_overflow_error);n<width;n++){
			topic[n]=line_overflow_error[n-width+strlen(line_overflow_error)];
		}
		topic[width]='\0';
		wprintw(channel_topic,topic);
	}
	
	//refresh the channel topic window
	wrefresh(channel_topic);
}

//display channel text as needed using current server and severs[current_server]->current_channel to tell what to display
void refresh_channel_text(){
	irc_connection *server=get_server(current_server);
	
	//if we're not connected to anything don't bother
	if(server==NULL){
		return;
	}
	
	//if an ncurses window doesn't exist for this display
	//then we can't display anything
	if(channel_text==NULL){
		return;
	}
	
	//get the relevant channel from the server's list of channels
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
	
	//number of messages in scrollback available
	int message_count;
	char **scrollback=ch->content;
	for(message_count=0;(message_count<MAX_SCROLLBACK)&&(scrollback[message_count]!=NULL);message_count++);
	
	int w_height,w_width;
	getmaxyx(channel_text,w_height,w_width);
	
	//print out the channel text
	//first clearing that window
	wblank(channel_text,width,height-RESERVED_LINES);
	
	//where to stop outputting, by default this is the last line available
	int output_end=message_count;
	
	//if we're scrolled up stop where we're scrolled to
	if(scrollback_end>=0){
		output_end=scrollback_end;
	}
	
	//account for word wrapping (figure out how many lines to ignore here)
	//store this in a structure because I'll be re-using it
	int overflow_lines[MAX_SCROLLBACK];
	int n;
	for(n=0;n<output_end;n++){
		overflow_lines[n]=0;
		if(strlen(scrollback[n])>w_width){
			//note this is integer division, which is floor of regular division
			overflow_lines[n]+=(strlen(scrollback[n])/width);
		}
	}
	
	int output_start=output_end;
	//available lines left to output to
	int lines_left=w_height;
	while((lines_left>0)&&(output_start>0)){
		output_start--;
		lines_left-=(overflow_lines[output_start]+1);
	}
	
	//account for if the first line is wrapped
	if(lines_left<0){
		output_start++;
	}
	
	//word wrap, and do it sanely
	int output_line;
	for(output_line=output_start;output_line<output_end;output_line++){
		//if there's text to display on this line
		if(scrollback[output_line]!=NULL){
			char output_text[BUFFER_SIZE];
			//start at the start of the line
//			wmove(channel_text,output_line-(output_end-w_height),0);
			int overflow_line_count=0;
			int n;
			for(n=output_start;n<output_line;n++){
				overflow_line_count+=overflow_lines[n];
			}
			int y_start=(output_line-output_start)+overflow_line_count;
			wmove(channel_text,y_start,0);
			
			//instead of a line overflow error, WRAP! (this is a straight-up character wrap)
			strncpy(output_text,scrollback[output_line],BUFFER_SIZE);
			
#ifdef MIRC_COLOR
			if(has_colors()){
				int wrapped_line=0;
				
				//handle MIRC colors
				//a data structure for colors that must persist outside the loop
				int colors[2];
				colors[FOREGROUND]=MIRC_WHITE;
				colors[BACKGROUND]=MIRC_BLACK;
				wcoloron(channel_text,MIRC_WHITE,MIRC_BLACK);
				
				char was_ping=FALSE;
				
				//timestamps are prepended to all lines
				//(unhandled lines no longer start with :, they start with <timestamp> :)
				char timestamp[BUFFER_SIZE];
				int space_index=strfind(" ",output_text);
				substr(timestamp,output_text,0,space_index);
				
				//if this line was a ping or included MIRC colors treat it specially (set attributes before output)
				int ping_check=strfind(server->nick,output_text);
				//if our name was in the message and we didn't send the message and it's not an unhandled message type (those start with ":")
				if((ping_check>=0)&&(strfind(">>",output_text)!=(space_index+1))&&(strfind(":",scrollback[output_line])!=(space_index+1))){
					was_ping=TRUE;
				}
				
				if(was_ping){
					wcoloron(channel_text,MIRC_GREEN,MIRC_BLACK);
				}
				
				//output the string a character at a time, taking into consideration MIRC colors
				int n;
				for(n=0;n<strlen(output_text);n++){
					//the CTCP escape is also output specially, as a bold "\\"
					if(output_text[n]==0x01){
						wattron(channel_text,A_BOLD);
						wprintw(channel_text,"\\");
						wattroff(channel_text,A_BOLD);
					//if this is not a special escape output it normally
					}else if(output_text[n]!=0x03){
						wprintw(channel_text,"%c",output_text[n]);
					}else{
						n++;
						int color_start=n;
						
						char input_background=FALSE;
						while((output_text[n]!='\0')&&(isdigit(output_text[n])||(output_text[n]==','))){
							//if we should start checking for a background color
							if((output_text[n]==',')&&(!input_background)){
								input_background=TRUE;
							//if we've already gotten a background color then this is the end of our handling, break the loop
							}else if(output_text[n]==','){
								break;
							//get the foreground
							}else if(!input_background){
								colors[FOREGROUND]*=10;
								colors[FOREGROUND]+=(output_text[n]-'0');
							//get the background
							}else{
								colors[BACKGROUND]*=10;
								colors[BACKGROUND]+=(output_text[n]-'0');
							}
							n++;
						}
						
						//if we never got a background
						if(!input_background){
							//treat it as MIRC code black
							colors[BACKGROUND]=MIRC_BLACK;
						}
						
						//if not one iteration of the loop was successful this is a reset escape, so reset
						if(color_start==n){
							wattrset(channel_text,0);
							wcoloron(channel_text,0,1);
							//and decrement n because the next character is something we want to display as a normal char
							n--;
						}else{
							if((colors[FOREGROUND]>=0)&&(colors[FOREGROUND]<MIRC_COLOR_MAX)&&(colors[BACKGROUND]>=0)&&(colors[BACKGROUND]<MIRC_COLOR_MAX)){
								//ignore anything previously set
								wattrset(channel_text,0);
								
								//okay, we know what we're setting now so set it and display
								wcoloron(channel_text,colors[FOREGROUND],colors[BACKGROUND]);
								wprintw(channel_text,"%c",output_text[n]);
//								wcoloroff(channel_text,colors[FOREGROUND],colors[BACKGROUND]);
							}else{
								wprintw(channel_text,"%c",output_text[n]);
							}
						}
					}
					
					if(((n+1)<strlen(output_text))&&((n+1)%width==0)){
						wrapped_line++;
						wmove(channel_text,(y_start+wrapped_line),0);
					}
				}
				
				if(was_ping){
					wcoloroff(channel_text,MIRC_GREEN,MIRC_BLACK);
				}
				
				//reset all attributes before we start outputting the next line in case they didn't properly terminate their colors
				wattrset(channel_text,0);
				wcoloron(channel_text,MIRC_WHITE,MIRC_BLACK);
			}else{
				int n;
#endif
				//whether or not we are displaying in bold
				char bold_on=FALSE;
				//instead of a line overflow error, WRAP! (this is a straight-up character wrap)
				int wrapped_line=0;
				for(n=0;n<strlen(output_text);n++){
					//output 0x03 here as bold '^' and 0x01 as bold '\' so they don't break line wrapping
					//the MIRC color code is not output like other characters (make it a bolded ^)
					if(output_text[n]==0x03){
						wattron(channel_text,A_BOLD);
						wprintw(channel_text,"^");
						if(!bold_on){
							wattroff(channel_text,A_BOLD);
						}
					//the CTCP escape is also output specially, as a bold "\\"
					}else if(output_text[n]==0x01){
						wattron(channel_text,A_BOLD);
						wprintw(channel_text,"\\");
						if(!bold_on){
							wattroff(channel_text,A_BOLD);
						}
					//a literal tab is output specially, as a bold "_", so that one character == one cursor position
					}else if(output_text[n]=='\t'){
						wattron(channel_text,A_BOLD);
						wprintw(channel_text,"_");
						if(!bold_on){
							wattroff(channel_text,A_BOLD);
						}
					//unicode support (requires -lncursesw)
					}else if((output_text[n] & 128)>0){
						//TODO: fix this; unicode handling is BROKEN and never worked that well to start with!
						
						//realistically a unicode character will only be like 4 or 5 bytes max
						//but modern systems have enough memory we can take a whole buffer
						//for just a second
						char utf8_char[BUFFER_SIZE];
						int utf_start=n;
						while((output_text[n] & 128)>0){
							utf8_char[(n-utf_start)]=output_text[n];
							n++;
						}
						//null-terminate
						utf8_char[n-utf_start]='\0';
						
						//display the unicode
						wprintw(channel_text,utf8_char);
						
						//pad the input area so cursor movement works nicely
						utf_start++;
						while(utf_start<n){
							wprintw(channel_text," ");
							utf_start++;
						}
						
						//because n++ occurs during the loop
						//decrement here to output correctly
						n--;
					//don't output a ^B, instead use it to toggle bold or not bold
					}else if(output_text[n]==0x02){
						if(bold_on){
							wattroff(channel_text,A_BOLD);
							bold_on=FALSE;
						}else{
							wattron(channel_text,A_BOLD);
							bold_on=TRUE;
						}
					//if this is not a special escape output it normally
					}else{
						wprintw(channel_text,"%c",output_text[n]);
					}
					
					if(((n+1)<strlen(output_text))&&((n+1)%width==0)){
						wrapped_line++;
						wmove(channel_text,(y_start+wrapped_line),0);
					}
				}
				//if we still had bold on at the end of output then turn it off now
				if(bold_on){
					wattroff(channel_text,A_BOLD);
				}
#ifdef MIRC_COLOR
			}
#endif
		}
	}
	//refresh the channel text window
	wrefresh(channel_text);
}

//refresh the user's input, duh
void refresh_user_input(char *input_buffer, int cursor_pos, int input_display_start){
	//if an ncurses window doesn't exist for this display
	//then we can't display anything
	if(user_input==NULL){
		return;
	}
	
	//output the most recent text from the user so they can see what they're typing
	wblank(user_input,width,1);
	wmove(user_input,0,0);
	
	int manual_offset=0;
	
	//if we can output the whole string just do that no matter what
	int length=strlen(input_buffer);
	if(length<width){
		input_display_start=0;
		if(cursor_pos>=width){
			cursor_pos=width-1;
		}
	}
	if(cursor_pos<width){
		input_display_start=0;
	}
	
	//if we're at the end of the line don't display the last char, leave that for the cursor
	if((cursor_pos-input_display_start)>=(width)){
		manual_offset=-1;
		input_display_start++;
	}
	
	int n;
	for(n=input_display_start;(n<(input_display_start+width+manual_offset))&&(n<BUFFER_SIZE);n++){
		//if we hit the end of the string before the end of the window stop outputting early
		if(input_buffer[n]!='\0'){
			//the MIRC color code is not output like other characters (make it a bolded ^)
			if(input_buffer[n]==0x03){
				wattron(user_input,A_BOLD);
				wprintw(user_input,"^");
				wattroff(user_input,A_BOLD);
			//the CTCP escape is also output specially, as a bold "\\"
			}else if(input_buffer[n]==0x01){
				wattron(user_input,A_BOLD);
				wprintw(user_input,"\\");
				wattroff(user_input,A_BOLD);
			//a ^B is used for bolding in some cases
			//output it as a bold !
			}else if(input_buffer[n]==0x02){
				wattron(user_input,A_BOLD);
				wprintw(user_input,"!");
				wattroff(user_input,A_BOLD);
			//a literal tab is output specially, as a bold "_", so that one character == one cursor position
			}else if(input_buffer[n]=='\t'){
				wattron(user_input,A_BOLD);
				wprintw(user_input,"_");
				wattroff(user_input,A_BOLD);
			//unicode support (requires -lncursesw)
			}else if((input_buffer[n] & 128)>0){
				//realistically a unicode character will only be like 4 or 5 bytes max
				//but modern systems have enough memory we can take a whole buffer
				//for just a second
				char utf8_char[BUFFER_SIZE];
				int utf_start=n;
				while((input_buffer[n] & 128)>0){
					utf8_char[(n-utf_start)]=input_buffer[n];
					n++;
				}
				//null-terminate
				utf8_char[n-utf_start]='\0';
				
				//display the unicode
				wprintw(user_input,utf8_char);
				
				//pad the input area so cursor movement works nicely
				utf_start++;
				while(utf_start<n){
					wprintw(user_input," ");
					utf_start++;
				}
				
				//because n++ occurs during the loop
				//decrement here to output correctly
				n--;
			//if this is not a special escape output it normally
			}else{
				wprintw(user_input,"%c",input_buffer[n]);
			}
		}else{
			n=input_display_start+width;
		}
	}
	wmove(user_input,0,cursor_pos-input_display_start);
	wrefresh(user_input);
}

//refresh the bottom bar above the input area
void refresh_statusbar(time_t *persistent_old_time, char *time_buffer, char *user_status_buffer){
	//if an ncurses window doesn't exist for this display
	//then we can't display anything
	if(bottom_border==NULL){
		return;
	}
	
	time_t old_time=(*persistent_old_time);

	//store the previous user status so we know if a change occurred
	char old_user_status_buffer[BUFFER_SIZE];
	strncpy(old_user_status_buffer,user_status_buffer,BUFFER_SIZE);
	
	//output for how many users are in the channel
	strncpy(user_status_buffer,"",BUFFER_SIZE);
	irc_connection *server=get_server(current_server);
	if(server!=NULL){
		if(server->current_channel>0){
			channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
			if(!ch->is_pm){
				//the reason for the -1 here is because the channel name is within the user_name list
				//for tab completion reasons
				//but shouldn't be considered a user for this status count
				snprintf(user_status_buffer,BUFFER_SIZE,"[%i users]-",(ch->nick_count)-1);
			}
		}
	}
	
	//output for when the user is scrolled up and by how much
	char scroll_status[BUFFER_SIZE];
	strncpy(scroll_status,"",BUFFER_SIZE);
	
	//if the user is scrolled up at all, give them some info
	if(scrollback_end>=0){
		snprintf(scroll_status,BUFFER_SIZE,"[scrolled to line %i]",scrollback_end);
	}else{
		strncpy(scroll_status,"[end]",BUFFER_SIZE);
	}
	
	if((scrollback_end!=prev_scrollback_end)||(strncmp(old_user_status_buffer,user_status_buffer,BUFFER_SIZE)!=0)){
		//this is to "trick" the time check into updating even when it otherwise wouldn't have
		old_time=0;
		strncpy(time_buffer,"",BUFFER_SIZE);
	}
	
	scroll_status[BUFFER_SIZE-1]='\0';
	
	//unix epoch clock in bottom_border, update it when the time changes
	time_t current_time=time(NULL);
	//if the time has changed
	if(current_time>old_time){
		//this logic sees if, even if the time changed, did the output to the user change!?
		char old_time_buffer[BUFFER_SIZE];
		strncpy(old_time_buffer,time_buffer,BUFFER_SIZE);
		custom_format_time(time_buffer,current_time);
		
		//only refresh the display if what the user sees changed
		//(if it's not displaying in seconds and only the seconds updated, don't refresh)
		if(strncmp(old_time_buffer,time_buffer,BUFFER_SIZE)!=0){
			wblank(bottom_border,width,1);
			
			wmove(bottom_border,0,0);
			wprintw(bottom_border,time_buffer);
			
			int n;
			for(n=strlen(time_buffer);n<(width-strlen(user_status_buffer)-strlen(scroll_status));n++){
				wprintw(bottom_border,"-");
			}
			wprintw(bottom_border,user_status_buffer);
			wprintw(bottom_border,scroll_status);
			//refresh the window from the buffer
			wrefresh(bottom_border);
			
			//re-set for next iteration
			old_time=current_time;
			
			//make sure the user doesn't see their cursor move
			wrefresh(user_input);
		}
	}
	
	(*persistent_old_time)=old_time;
}


void scrollback_output(int server_index, int output_channel, char *to_output, char refresh){
	char output_buffer[BUFFER_SIZE];
	strncpy(output_buffer,to_output,BUFFER_SIZE);
	
	//regardless of what our output was, timestamp it
	//for logging, always use the unix timestamp
	char log_buffer[BUFFER_SIZE];
	snprintf(log_buffer,BUFFER_SIZE,"%ju %s",(uintmax_t)(time(NULL)),output_buffer);
	
	//for outputting to the user in ncurses, use a custom time format (by default unix timestamp)
	char time_buffer[BUFFER_SIZE];
	char custom_time[BUFFER_SIZE];
	custom_format_time(custom_time,time(NULL));
	
	snprintf(time_buffer,BUFFER_SIZE,"%s %s",custom_time,output_buffer);
	strncpy(output_buffer,time_buffer,BUFFER_SIZE);
	
	//add the message to the relevant channel scrollback structure
	irc_connection *server=get_server(server_index);
	if(server==NULL){
		//we can't output and scrollback if there is no server
		return;
	}
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,output_channel)->data);
	
	char **scrollback=ch->content;
	
	//find the next blank line
	int scrollback_line;
	for(scrollback_line=0;(scrollback_line<MAX_SCROLLBACK)&&(scrollback[scrollback_line]!=NULL);scrollback_line++);
	
	//if we filled the buffer move everything back and take the last line
	if(scrollback_line>=MAX_SCROLLBACK){
		//free the line we're pushing out of the buffer
		free(scrollback[0]);
		//move all other lines back
		for(scrollback_line=0;scrollback_line<(MAX_SCROLLBACK-1);scrollback_line++){
			scrollback[scrollback_line]=scrollback[scrollback_line+1];
		}
		//put in the new line at the end
		scrollback_line=MAX_SCROLLBACK-1;
	}
	//regardless of if we filled the buffer add in the new line here
	scrollback[scrollback_line]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
	
	//NOTE: this is an output buffer and not the raw read buffer because we might not always output /exactly/ what we got from the server
	//but we don't want the read_buffer changed above
//	strncpy(scrollback[scrollback_line],server->read_buffer,BUFFER_SIZE);
	strncpy(scrollback[scrollback_line],output_buffer,BUFFER_SIZE);
	
	//defensively null-terminate, just in case
	scrollback[scrollback_line][BUFFER_SIZE-1]='\0';
	
	//indicate that there is new text if the user is not currently in this channel
	//through the channel_list and server_list
	ch->new_content=TRUE;
	if(refresh){
		refresh_channel_list();
		refresh_server_list();
	}
	
	//if we're keeping logs write to them
	if((server->keep_logs)&&(ch->log_file!=NULL)){
		fprintf(ch->log_file,"%s\n",log_buffer);
	}
	
	//if this was currently in view update it there
	if((current_server==server_index) && (server->current_channel==output_channel) && (refresh)){
		refresh_channel_text();
	}
}

void leave_channel(int server_index, char *ch_name){
	irc_connection *server=get_server(server_index);
	
	//go through the channels, find out the one to remove from our structures
	int channel_index=ch_idx_from_name(server,ch_name);
	//if we found the channel to leave
	if(channel_index>=0){
		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,channel_index)->data);
		
		//free any memory and clear structures
		strncpy(ch->name,"",BUFFER_SIZE);
		strncpy(ch->topic,"",BUFFER_SIZE);
		
		int n;
		for(n=0;n<MAX_SCROLLBACK;n++){
			if(ch->content[n]!=NULL){
				free(ch->content[n]);
				ch->content[n]=NULL;
			}
		}
		
		//if we were keeping logs close them
		if((server->keep_logs)&&(ch->log_file!=NULL)){
			fclose(ch->log_file);
			ch->log_file=NULL;
		}
		
		//if we were in this channel kick back to the reserved SERVER channel
		if(server->current_channel==channel_index){
			server->current_channel=0;
		}
		
		//remove this item from the doubly-linked list of channels (freeing the associated memory)
		server->ch=dlist_delete_entry(server->ch,channel_index,TRUE);

		//and refresh the channel list
		refresh_channel_list();
	}
}

//add a name to the names list for a channel
void add_name(int server_index, int channel_index, char *name, const char *mode_prefix){
	irc_connection *server=get_server(server_index);
	if(server==NULL){
		return;
	}
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,channel_index)->data);
	
	//check if this user is already in the list for this channel
	int matches=0;
	
	int idx=0;
	while(idx>=0){
		idx=nick_idx(ch,name,idx);
		
		//found this nick
		if(idx>=0){
			matches++;
			
			//if it was a duplicate
			if(matches>1){
				//then remove this copy
				ch->user_names=dlist_delete_entry(ch->user_names,idx,TRUE);
				
				//NOTE: this changes indexes so our next search must start at 0 to ensure we don't miss anything
				idx=0;
				//any previous matches are void since we invalidated indices and are re-starting the search from the start of the list
				matches=0;
				
				//now that we removed this entry there is one fewer nick
				//so update the associated count
				ch->nick_count--;
			}else{
				//if this was NOT a duplicate then start the search from the next list index
				//to avoid double-counting
				idx++;
			}
		}
	}
	
	//if the user wasn't already there
	if(matches==0){
		//find a spot for a new user
		nick_info *nick_content=(nick_info *)(malloc(sizeof(nick_info)));
		strncpy(nick_content->user_name,name,BUFFER_SIZE);
		set_mode_str_from_prefix(nick_content->mode_str,mode_prefix,BUFFER_SIZE);
		ch->user_names=dlist_append(ch->user_names,nick_content);
		ch->nick_count++;
	//if this user did already exist
	}else if(matches==1){
		idx=nick_idx(ch,name,0);
//		dlist_entry *nick_entry=dlist_get_entry(ch->user_names,idx);
//		nick_info *nick_content=(nick_info*)(nick_entry->data);

		//NOTE: if user already existed,
		//we do NOT change the mode string
		//and instead use the existing mode string for this user in this channel
		//which will already be correct as long as our other mode handling is working
	}
}

//remove a name to the names list for a channel
void del_name(int server_index, int channel_index, char *name){
	irc_connection *server=get_server(server_index);
	if(server==NULL){
		return;
	}
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,channel_index)->data);
	
	//remove this user from the names array of the channel she/he parted
	int idx=0;
	while(idx>=0){
		idx=nick_idx(ch,name,0);
		
		//found this nick
		if(idx>=0){
			//remove this user from that channel's names array
			ch->user_names=dlist_delete_entry(ch->user_names,idx,TRUE);

			//update the user count to note that we just lost a user
			ch->nick_count--;
		}
	}
}

//check if the user was pingged in the given text
//returns earliest index >=0 of ping phrase found; -1 if no ping occurred
int ping_phrase_check(char *lwr_nick, dlist_entry *ping_phrases, char *chk_text){
	//first check for the user's nick; this is always a ping, no matter what
	int ping_idx=strfind(lwr_nick,chk_text);
	int min_ping_idx=ping_idx;
	
	//then check for anything that user explicity set as a ping
	dlist_entry *ping_phrase_entry=ping_phrases;
	while(ping_phrase_entry!=NULL){
		char *ping_phrase=(char *)(ping_phrase_entry->data);
		
		//look for the ping phrase in the text string
		ping_idx=strfind(ping_phrase,chk_text);
		
		//if the ping phrase was found
		if(ping_idx>=0){
			//if no other phrase had been found so far, or this phrase was earlier
			//then this is the min so start using that
			if((min_ping_idx<0) || (ping_idx<min_ping_idx)){
				min_ping_idx=ping_idx;
			}
		}
		
		ping_phrase_entry=ping_phrase_entry->next;
	}
	
	//return first index of a ping phrase
	return min_ping_idx;
}


//BEGIN parse_input HELPER FUNCTIONS

//add a server to the list (called from the client connect command)
//returns a pointer to the new server structure
irc_connection *add_server(int new_socket_fd, char *host, int port){
	irc_connection *server=(irc_connection*)(malloc(sizeof(irc_connection)));
	
	//zero out the server structure to ensure no uninitialized data
	memset(server,0,sizeof(irc_connection));
	
	//add this server to the doubly-linked list of servers
	int server_index=dlist_length(servers);
	servers=dlist_append(servers,server);
	
#ifdef _OPENSSL
	//by default don't use ssl; this will be set by sconnect as needed
	server->use_ssl=FALSE;
	
	//clear out ssl-specific structures
	server->ssl_handle=NULL;
	server->ssl_context=NULL;
#endif
	
	//initialize the buffer to all NULL bytes
	int n;
	for(n=0;n<BUFFER_SIZE;n++){
		server->read_buffer[n]='\0';
	}
	server->socket_fd=new_socket_fd;
	
	//initially we are not in the middle of a line and the parse queue is empty
	strncpy(server->parse_queue,"",BUFFER_SIZE);
	
	//initially the last message was received from the server like right now
	server->last_msg_time=time(NULL);
	
	//set the port information (in case we need to re-connect)
	server->port=port;
	
	//set the server name
	strncpy(server->server_name,host,BUFFER_SIZE);

	//by default don't ping on pms, other than the first one
	server->ping_on_pms=FALSE;
	
	//by default don't hide joins and quits
	server->hide_joins_quits=FALSE;
	
	//by default don't hide any user mode changes
	strncpy(server->hidden_user_mode_changes,"",BUFFER_SIZE);
	
	//set the current channel to be 0 (the system/debug channel)
	//a JOIN would add channels, but upon initial connection 0 is the only valid one
	server->current_channel=0;
	
	//set the default channel for various messages from the server that are not channel-specific
	channel_info *ch=(channel_info *)(malloc(sizeof(channel_info)));
	strncpy(ch->name,"SERVER",BUFFER_SIZE);
	strncpy(ch->topic,"(no topic for the server)",BUFFER_SIZE);
	
	//set the main chat window with scrollback
	//as we get lines worth storing we'll add them to this content, but for the moment it's blank
	for(n=0;n<MAX_SCROLLBACK;n++){
		ch->content[n]=NULL;
	}
	
	//by default don't reconnect if connnection is lost
	server->reconnect=FALSE;
	
	//note that keeping logs is true by default, but can be set with client commands log and no_log
	if(can_log){
		server->keep_logs=TRUE;
	//if we are unable to log due to e.g. permission errors, don't even try
	}else{
		server->keep_logs=FALSE;
	}
	
	if(server->keep_logs){
		//TODO: create a subdirectory for the user's nick so that multiple logs can be kept for simultaneous connections with different nicks
		//the directory structure should then be ~/.local/share/accirc/logs/<server>/<nick>/ with all the #channel files under that
		//NOTE: this is complicated because the user's nick can change ove time so for now that's just not being done
		
		//first make a directory for this server
		char file_location[BUFFER_SIZE];
		snprintf(file_location,BUFFER_SIZE,"%s/.local/share/accirc/%s/%s/%s",getenv("HOME"),LOGGING_DIRECTORY,server->server_name,ch->name);
		
		if(verify_or_make_parent_dirs_for(file_location)){
			//note that if this call fails it will be set to NULL and hence be skipped over when writing logs
			ch->log_file=fopen(file_location,"a");
			if(ch->log_file!=NULL){
				//turn off buffering since I need may this output immediately and buffers annoy me for that
				setvbuf(ch->log_file,NULL,_IONBF,0);
			}else{
				scrollback_output(server_index,0,"accirc: Err: could not make log file",TRUE);
			}
		//this fails in a non-silent way, the user should know there was a problem
		//if we couldn't make the directory then don't keep logs rather than failing hard
		}else{
			scrollback_output(server_index,0,"accirc: Err: could not make logging directory",TRUE);
			server->keep_logs=FALSE;
		}
	}
	
	//by default don't rejoin on kick
	server->rejoin_on_kick=FALSE;
	
	//by default the fallback nick is null
	strncpy(server->fallback_nick,"",BUFFER_SIZE);
	
	//there are no users in the SERVER channel
	ch->user_names=NULL;
	ch->nick_count=0;
	
	//channel content for server is empty, as is ping state
	ch->new_content=FALSE;
	ch->was_pingged=FALSE;
	
	//set the list of channels to be a single entry (the pre-configured "SERVER" data channel)
	//no other initialization needs to be done until a channel is joined
	server->ch=dlist_append(server->ch,ch);
	
	//default channel order to null, meaning view channels in the order they were joined
	server->channel_order=NULL;
	
	//clear out the post information
	strncpy(server->post_type,"",BUFFER_SIZE);
	server->post_commands=NULL;
	
	//initially at least there are no lines to run
	//upon server connection completing
	server->server_run_lines=NULL;
	
	//by default there is no new content on this server
	//(because new server content is the OR of new channel content, and by default there is no new channel content)
	
	//default the user's name to NULL until we get more information (NICK data)
	strncpy(server->nick,"",BUFFER_SIZE);
	
	//by default no one has PM'd us so a reply goes to a blank nick
	//should this be another default? it's just that the next word may be interpreted as the nick...
	//screw it, I'll document it in the man, it'll be considered intended behavior
	strncpy(server->last_pm_user,"",BUFFER_SIZE);
	
	//by default the quit message for this server is the generic quit string constant
	strncpy(server->quit_msg,DEFAULT_QUIT_MESSAGE,BUFFER_SIZE);
	
	//by default don't show user mode strings
	server->use_mode_str=FALSE;
	
	//set the current server to be the one we just connected to
	current_server=server_index;
	
	//and return that for later reference
	return server;
}

//a function to add a line to the user's input history, to later (possibly) be scrolled back to
void add_history_entry(char *input_buffer){
	//add this line to the user's input history (last entry)
	//note input_line can be used here since we're re-setting it after a user input anyway
	for(input_line=0;(input_line<MAX_SCROLLBACK)&&(input_history[input_line]!=NULL);input_line++);
	//if we're out of history scrollback clear out the first entry from there
	if(input_line>=MAX_SCROLLBACK){
		//free the line we're pushing out of the buffer
		free(input_history[0]);
		//move all other lines back
		for(input_line=0;input_line<(MAX_SCROLLBACK-1);input_line++){
			input_history[input_line]=input_history[input_line+1];
		}
		//put in the new line at the end
		input_line=MAX_SCROLLBACK-1;
	}
	//regardless of if we filled the buffer add in the new line here
	input_history[input_line]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
	
	strncpy(input_history[input_line],input_buffer,BUFFER_SIZE);
	
	//set input_line to -1 (indicating current input); so if the user was scrolled up in the history they're now at the end
	input_line=-1;
}

//a helper function to output to the correct channel based on a given channel name
int find_output_channel(irc_connection *server, char *channel){
	if(server==NULL){
		return 0;
	}
	
	int output_channel=0;
	
	int channel_index=ch_idx_from_name(server,channel);
	//if this channel is anything more specific than the reserved server channel
	if(channel_index>=0){
		//then output there
		output_channel=channel_index;
	}
	//else output to the reserved server channel
	
	return output_channel;
}

//a helper function for join_new_channel
//which applies the channel order as specified in the server->channel_order dlist
//using dlist_swap after append because it seems like I currently don't have a dlist_insert function
int apply_channel_order(irc_connection *server, const char *channel){
	//if we don't have enough connected channels for ordering to make sense
	if(dlist_length(server->ch)<2){
		//then the active channel is just the last one in the list
		//and we perform no move operations
		return dlist_length(server->ch)-1;
	}
	
	//get the current index of this channel based on its name
	int new_ch_idx=ch_idx_from_name(server,channel);
	//if this channel isn't currently in the channel list
	//and is therefore not joined
	if(new_ch_idx<0){
		//then the active channel is just the last one in the list
		//and we perform no move operations
		return dlist_length(server->ch)-1;
	}
	
	//if there wasn't an explicit order specified for the new channel
	//then it goes at the end, and we do no move operations
	int new_ch_order_idx=ch_order_from_name(server,channel);
	if(new_ch_order_idx<0){
		//so return what we were given
		return new_ch_idx;
	}
	
	//NOTE: we intentionally always skip the SERVER channel, which should be always at index 0
	//NOTE: we are counting backwards because this is essentially an online insertion sort
	//and yes I know merge sort is faster but for our data structures and the number of items we have
	//this is fine for now
	for(int existing_ch_idx=dlist_length(server->ch)-1;existing_ch_idx>0;existing_ch_idx--){
		int order_idx=ch_order_from_name(server,((channel_info *)(dlist_get_entry(server->ch,existing_ch_idx)->data))->name);
		
		//if an order_idx was present and the new_ch_order_idx says the new channel goes before this
		//OR no order was specified for this channel
		//(but per earlier checks we know that for the new channel an order WAS specified)
		if(((order_idx>=0) && (new_ch_order_idx<order_idx)) || (order_idx<0)){
			//swap the items at existing_ch_idx and new_ch_idx
			server->ch=dlist_swap(server->ch,existing_ch_idx,new_ch_idx);
			
			//after the swap, the newly joined channel is now at the index
			//that previously held the existing channel
			//so update the new_ch_idx in case we need to do more swaps
			//in the case that this item should be moved ahead of multiple other channels
			new_ch_idx=existing_ch_idx;
		}
	}
	
	//after doing whatever swaps are needed
	//return the now-updated new channel index
	//where the channel can now be found
	return new_ch_idx;
}

//join a new channel (used when join is received from the server, and for the hi command)
//the pm_flag dictates whether this will be a PM or normal channel
void join_new_channel(int server_index, char *channel, char *output_buffer, int *output_channel, char pm_flag){
	irc_connection *server=get_server(server_index);
	if(server==NULL){
		//we can't join a channel on a non-existant server
		return;
	}
	
	//due to bouncer cases it's worth checking to verify we're not already in this channel

	int channel_index=ch_idx_from_name(server,channel);
	if(channel_index>=0){
		scrollback_output(server_index,channel_index,"accirc: Warn: server sent an extra JOIN; ignoring...",TRUE);
		return;
	}
	
	//the channel that we are joining will be at the end of the server's list of channels
	//so update that index accordingly
	channel_index=dlist_length(server->ch);
	
	channel_info *ch=(channel_info *)(malloc(sizeof(channel_info)));
	
	//initialize the channel name to be what was joined
	strncpy(ch->name,channel,BUFFER_SIZE);
	
	//default to a null topic
	strncpy(ch->topic,"",BUFFER_SIZE);
	
	//null out the content to start with
	int n;
	for(n=0;n<MAX_SCROLLBACK;n++){
		ch->content[n]=NULL;
	}
	
	ch->user_names=NULL;
	ch->nick_count=0;
	ch->is_pm=pm_flag;
	
	//if we should be keeping logs make sure we are
	if(server->keep_logs){
		//TODO: use a subdirectory for the user's nick so that multiple logs can be kept for simultaneous connections with different nicks
		//the directory structure should then be ~/.local/share/accirc/logs/<server>/<nick>/ with all the #channel files under that
		//NOTE: this is complicated because the user's nick can change ove time so for now that's just not being done
		
		char file_location[BUFFER_SIZE];
		snprintf(file_location,BUFFER_SIZE,"%s/.local/share/accirc/%s/%s/%s",getenv("HOME"),LOGGING_DIRECTORY,server->server_name,ch->name);
		
		//NOTE: we declare this error message here so it doesn't need to be repeated
		//but it is only actually output if there is actually an error
		char error_buffer[BUFFER_SIZE];
		snprintf(error_buffer,BUFFER_SIZE,"accirc: Err: could not make log file for channel %s",ch->name);
		
		//if this channel name contains any '/' characters make the associated subdirectories at this time
		if(verify_or_make_parent_dirs_for(file_location)){
			//note if this fails it will be set to NULL and hence will be skipped over when trying to output to it
			ch->log_file=fopen(file_location,"a");
			if(ch->log_file!=NULL){
				//turn off buffering since I need may this output immediately and buffers annoy me for that
				setvbuf(ch->log_file,NULL,_IONBF,0);
			}else{
				scrollback_output(server_index,channel_index,error_buffer,TRUE);
			}
		}else{
			scrollback_output(server_index,channel_index,error_buffer,TRUE);
		}
	}
		
	//add this channel to the server-wide channel list
	server->ch=dlist_append(server->ch,ch);
	
	//apply the server->channel_order setting here during insert
	//using dlist_swap after append because it seems like I currently don't have a dlist_insert function
	channel_index=apply_channel_order(server,channel);
	
	//set this to be the current channel, we must want to be here if we joined
	server->current_channel=channel_index;
	
	//if this is a PM opening we won't have the normal join, so add in a message to let the user know about it
	if(pm_flag){
		snprintf(output_buffer,BUFFER_SIZE,"accirc: Joining new (faux) PM channel");
	}
	
	//output the join at the top of this channel, why not
	*output_channel=channel_index;
	
	//add the channel name to the nick list (channels and nicks are mutually exclusive so this is fine)
	add_name(server_index,channel_index,channel,"");
	
	//and refresh the channel list
	refresh_channel_list();
}


//the "connect" client command handling, and "sconnect" via the ssl paramater (parsed out earlier)
void connect_command(char *input_buffer, char *command, char *parameters, char ssl){
	//if we were listening for post before, we aren't now, this is a new server!
	post_listen=FALSE;
	
	char host[BUFFER_SIZE];
	char port_buffer[BUFFER_SIZE];
	int first_space=strfind(" ",parameters);
	if(first_space<0){
		//handle for insufficient parameters
		//note text output cannot be done here (except maybe to stdout or stderr) since we may not be connected to any server
		int n;
		for(n=0;n<3;n++){
			beep();
			usleep(100000);
		}
		
		//write this to the error log, so the user can view it if they choose
		char error_buffer[BUFFER_SIZE];
		snprintf(error_buffer,BUFFER_SIZE,"Err: too few arguments given to \"%s\"",command);
		error_log(error_buffer);
	}else{
		substr(host,parameters,0,first_space);
		substr(port_buffer,parameters,first_space+1,strlen(parameters)-(first_space+1));
		int port=0;
		
		//parse optional own_ip argument to this command
		//and convert it to a struct in_addr pointer
		//to pass it as the last argument to the make_socket_connection function
		struct in_addr *own_ip=NULL;
		int first_space=strfind(" ",port_buffer);
		if(first_space<0){
			port=atoi(port_buffer);
		}else{
			substr(parameters,port_buffer,first_space+1,strlen(port_buffer)-(first_space+1));
			substr(port_buffer,port_buffer,0,first_space);
			port=atoi(port_buffer);
			
#ifdef DEBUG
			char error_buffer[BUFFER_SIZE];
			snprintf(error_buffer,BUFFER_SIZE,"dbg: attempting to set own_ip to \"%s\" while connecting to \"%s\" on port \"%u\"",parameters,host,port);
			error_log(error_buffer);
#endif
			
			own_ip=malloc(sizeof(struct in_addr));
			memset(own_ip,'\0',sizeof(struct in_addr));
			inet_aton(parameters,own_ip);
		}
		
		//handle a new connection
		int new_socket_fd=make_socket_connection(input_buffer,host,port,own_ip);
		
		//clean up the own_ip struct now that we're done with it
		if(own_ip!=NULL){
			free(own_ip);
			own_ip=NULL;
		}
		
		//if the connection was unsuccessful give up
		if(new_socket_fd==-1){
			return;
		}
		
		//make some data structures for relevant information
		//add the server (this updates the global servers doubly-linked list with the relevant new server information)
		irc_connection *server=add_server(new_socket_fd,host,port);
		
		//NOTE: if ssl support is not compiled in the ssl parameter of this function is just totally ignored
		//(this is intentional behavior)
#ifdef _OPENSSL
		//if this is an ssl connection, do some handshaking (the socket is known to be valid if we didn't return by now)
		if(ssl){
			//a string buffer to store any errors that might occur
			char ssl_error_buffer[BUFFER_SIZE];
			
			//remember we're using SSL, this will be important for all reads and writes
			server->use_ssl=TRUE;
			
			//store the global cert check setting with this server when the connection is made
			server->ssl_cert_check=ssl_cert_check;
			
			//register error strings for libcrypto and libssl
			SSL_load_error_strings();
			//register ciphers and digests
			SSL_library_init();
			
			//new context stating we are client using SSL 2 or SSL 3
			server->ssl_context=SSL_CTX_new(SSLv23_client_method());
			if(server->ssl_context==NULL){
				snprintf(ssl_error_buffer,BUFFER_SIZE,"Err: SSL connection to host %s on port %i failed (context error)",host,port);
				error_log(ssl_error_buffer);
				properly_close(current_server);
				return;
			}
			
			//create an ssl struct for the connection based on the above context
			server->ssl_handle=SSL_new(server->ssl_context);
			if(server->ssl_handle==NULL){
				snprintf(ssl_error_buffer,BUFFER_SIZE,"Err: SSL connection to host %s on port %i failed (handle error)",host,port);
				error_log(ssl_error_buffer);
				properly_close(current_server);
				return;
			}
			
			//associate the SSL struct with the socket
			if(!SSL_set_fd(server->ssl_handle,new_socket_fd)){
				snprintf(ssl_error_buffer,BUFFER_SIZE,"Err: SSL connection to host %s on port %i failed (set_fd error)",host,port);
				error_log(ssl_error_buffer);
				properly_close(current_server);
				return;
			}

			//try to make an error log file, if that's impossible just use stderr
			const char* ca_path_name="/etc/ssl/certs";
			int ssl_verify_state=SSL_VERIFY_PEER;
			
			//trust all the same cert authorities that are configured to be default-trusted by openssl at /etc/ssl/certs
			if((ssl_cert_check==TRUE) && (SSL_CTX_load_verify_locations(server->ssl_context,NULL,ca_path_name))){
#ifdef DEBUG
				//set certificate checking settings prior to handshake
				snprintf(ssl_error_buffer,BUFFER_SIZE,"Info: Checking SSL cert of %s using files at %s\n",host,ca_path_name);
				scrollback_output(current_server,0,ssl_error_buffer,TRUE);
#endif
			}else{
				//this is a read error if we are configured to check certs
				//otherwise it's an expected configuration, so don't output an error
				if(ssl_cert_check==TRUE){
					snprintf(ssl_error_buffer,BUFFER_SIZE,"Err: Could not open the cert authority directory at %s; not verifying SSL!",ca_path_name);
					error_log(ssl_error_buffer);
					scrollback_output(current_server,0,ssl_error_buffer,TRUE);
				}
				ssl_verify_state=SSL_VERIFY_NONE;
			}
			SSL_set_verify(server->ssl_handle,ssl_verify_state,NULL);
			
			SSL_set_verify_depth(server->ssl_handle,6);
			
			//do the SSL handshake
			if(SSL_connect(server->ssl_handle)!=1){
				snprintf(ssl_error_buffer,BUFFER_SIZE,"Err: SSL connection to host %s on port %i failed (handshake error)",host,port);
				error_log(ssl_error_buffer);
				properly_close(current_server);
				return;
			}
			
			int ssl_verify_result=SSL_get_verify_result(server->ssl_handle);
			if(ssl_verify_result!=X509_V_OK){
				
				snprintf(ssl_error_buffer,BUFFER_SIZE,"Warn: SSL cert check failed for host %s on port %i, ssl_verify_result=%i",host,port,ssl_verify_result);
				//TODO: generalize this if and include information for all other errors
				//the lookup table for this can be found (at time of writing) at: https://github.com/openssl/openssl/blob/master/include/openssl/x509_vfy.h.in
				if(ssl_verify_result==X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT){
					snprintf(ssl_error_buffer,BUFFER_SIZE,"Warn: SSL cert check failed for host %s on port %i, ssl_verify_result=%i (SELF-SIGNED CERT)",host,port,ssl_verify_result);
				}
				error_log(ssl_error_buffer);
				scrollback_output(current_server,0,ssl_error_buffer,TRUE);
#ifdef DEBUG
			}else{
				snprintf(ssl_error_buffer,BUFFER_SIZE,"Info: SSL cert check SUCCEEDED for host %s on port %i, ssl_verify_result=%i\n",host,port,ssl_verify_result);
				scrollback_output(current_server,0,ssl_error_buffer,TRUE);
#endif
			}
		}
#endif
		
		//set the socket non-blocking
		int flags=fcntl(new_socket_fd,F_GETFL,0);
		flags=(flags==-1)?0:flags;
		fcntl(new_socket_fd,F_SETFL,flags|O_NONBLOCK);
		
		//if we're in "easy mode" then auto-send the user quartet so the user doesn't have to
		//THIS MUST BE DONE AFTER THE SSL HANDSHAKE or it won't work for secure connections
		if(easy_mode){
			char easy_mode_buf[BUFFER_SIZE];
			
			//the syntax for USER is "USER <username> <hostname> <servername> <realname>"
			snprintf(easy_mode_buf,BUFFER_SIZE,"%cuser %s %s %s %s",server_escape,DEFAULT_NICK,host,host,DEFAULT_NICK);
			parse_input(easy_mode_buf,FALSE);
			
			//default the nick to accirc_user too, just to get connected (the user can always change this later)
			snprintf(easy_mode_buf,BUFFER_SIZE,"%cnick %s",server_escape,DEFAULT_NICK);
			parse_input(easy_mode_buf,FALSE);
		}
		
		//output the server information (note we set current_server to the new index in add_server())
		refresh_server_list();
	}
}

//the "exit" client command
void exit_command(char *input_buffer, char *command, char *parameters){
	done=TRUE;
	
	char quit_message[BUFFER_SIZE];
	
	int n;
	int server_count=dlist_length(servers);
	for(n=0;n<server_count;n++){
		irc_connection *server=get_server(n);
		
		if(!strcmp(parameters,"")){
			//TODO: handle overflow case if quit_msg is longer than can fit
			//^ I think set command string length solves this implicitly?
			snprintf(quit_message,BUFFER_SIZE,"QUIT :%s\n",server->quit_msg);
		}else{
			snprintf(quit_message,BUFFER_SIZE,"QUIT :%s\n",parameters);
		}
		
		server_write(n,quit_message);
	}
}

//the "cli_escape" client command
void cli_escape_command(char *input_buffer, char *command, char *parameters){
	if(!strcmp(parameters,"")){
		//too few arguments given, do nothing (give up)
		
		//tell the user there were too few arguments
		char error_buffer[BUFFER_SIZE];
		snprintf(error_buffer,BUFFER_SIZE,"accirc: Err: too few arguments given to \"%s\"",command);
		
		//use the ncurses UI if possible; if not fall back to the error log file
		if(current_server>=0){
			scrollback_output(current_server,0,error_buffer,TRUE);
		}else{
			error_log(error_buffer);
		}
	}else{
		//we got an argument, since this is always a 1-character escape take the first char
		char tmp_client_escape=parameters[0];
		
		//a buffer to use when replying to the user
		char reply_buffer[BUFFER_SIZE];
		
		//if this isn't already the server escape
		if(tmp_client_escape!=server_escape){
			//set it to be the new client escape (discarding the old one)
			client_escape=tmp_client_escape;
			
			//tell the user we set the escape
			snprintf(reply_buffer,BUFFER_SIZE,"accirc: client escape set to \"%c\"",client_escape);
		}else{
			snprintf(reply_buffer,BUFFER_SIZE,"accirc: Err: client escape could not be set to \"%c\", that escape is already in use",tmp_client_escape);
		}
		
		//use the ncurses UI if possible; if not fall back to the error log file
		if(current_server>=0){
			scrollback_output(current_server,0,reply_buffer,TRUE);
		}else{
			error_log(reply_buffer);
		}
	}
}

//the "ser_escape" client command
void ser_escape_command(char *input_buffer, char *command, char *parameters){
	if(!strcmp(parameters,"")){
		//too few arguments given, do nothing (give up)
		
		//tell the user there were too few arguments
		char error_buffer[BUFFER_SIZE];
		snprintf(error_buffer,BUFFER_SIZE,"accirc: Err: too few arguments given to \"%s\"",command);
		
		//use the ncurses UI if possible; if not fall back to the error log file
		if(current_server>=0){
			scrollback_output(current_server,0,error_buffer,TRUE);
		}else{
			error_log(error_buffer);
		}
	}else{
		//we got an argument, since this is always a 1-character escape take the first char
		char tmp_server_escape=parameters[0];
		
		//a buffer to use when replying to the user
		char reply_buffer[BUFFER_SIZE];
		
		//if this isn't already the client escape
		if(tmp_server_escape!=client_escape){
			//set it to be the new server escape (discarding the old one)
			server_escape=tmp_server_escape;
			
			//tell the user we set the escape
			snprintf(reply_buffer,BUFFER_SIZE,"accirc: server escape set to \"%c\"",server_escape);
		}else{
			snprintf(reply_buffer,BUFFER_SIZE,"accirc: Err: server escape could not be set to \"%c\", that escape is already in use",tmp_server_escape);
		}
		
		//use the ncurses UI if possible; if not fall back to the error log file
		if(current_server>=0){
			scrollback_output(current_server,0,reply_buffer,TRUE);
		}else{
			error_log(reply_buffer);
		}
	}
}

//the "alias" client command (registers a new alias in the substitution array)
void alias_command(char *input_buffer, char *command, char *parameters){
	//make a buffer in case we want to do output
	char output_buffer[BUFFER_SIZE];
	
	//the first space-delimited item in parameters is the trigger to check
	char trigger[BUFFER_SIZE];
	char substitution[BUFFER_SIZE];
	int first_space=strfind(" ",parameters);
	
	//error handling, don't get ahead of ourselves
	if(first_space<0){
		//write this to the error log, so the user can view it if they choose
		snprintf(output_buffer,BUFFER_SIZE,"Err: too few arguments given to \"%s\"",command);
		error_log(output_buffer);
		if(current_server>=0){
			snprintf(output_buffer,BUFFER_SIZE,"accirc: Err: too few arguments given to \"%s\" (put a space at the end if you meant to delete this alias)",command);
			scrollback_output(current_server,0,output_buffer,TRUE);
		}
		return;
	}
	
	//set (local) trigger and substitution appropriately (what the user asked us to set)
	substr(trigger,parameters,0,first_space);
	substr(substitution,parameters,first_space+strlen(" "),strlen(parameters)-first_space-strlen(" "));
	
	//until we find an alias we say it's new
	int found_alias_idx=-1;
	
	//go through all aliased commands, see if this alias already exists
	dlist_entry *alias_list_entry=alias_list;
	alias *alias_item=NULL;
	int idx=0;
	while(alias_list_entry!=NULL){
		alias_item=(alias *)(alias_list_entry->data);
		//NOTE: aliases are case-sensitive (intentionally)
		if(!strncmp(trigger,alias_item->trigger,BUFFER_SIZE)){
			found_alias_idx=idx;
			break;
		}
		
		idx++;
		alias_list_entry=alias_list_entry->next;
	}
	
	//if we already had an alias for this, just change that one
	if(found_alias_idx>=0){
		if(!strcmp(substitution,"")){
			//NOTE: we don't call dlist_free_entry directly because it might be the 0th item that gets deleted
			//and in that case we need to get updated pointers
			alias_list=dlist_delete_entry(alias_list,found_alias_idx,TRUE);
		}else{
			//NOTE: because we did a break in the above loop, alias_item is still set to the list item at found_alias_idx
			//so we don't need to find it again
			strncpy(alias_item->substitution,substitution,BUFFER_SIZE);
		}
	//if it's a new alias then append it to the alias list
	}else{
		alias_item=(alias*)(malloc(sizeof(alias)));
		strncpy(alias_item->trigger,trigger,BUFFER_SIZE);
		strncpy(alias_item->substitution,substitution,BUFFER_SIZE);
		
		alias_list=dlist_append(alias_list,alias_item);
	}
	
	//if possible, tell the user what's going on (if it's not possible to output, still set the alias, just be silent)
	if(current_server>=0){
		if(!strcmp(substitution,"")){
			snprintf(output_buffer,BUFFER_SIZE,"accirc: deleted alias for \"%s\"",trigger);
		}else{
			snprintf(output_buffer,BUFFER_SIZE,"accirc: setting alias \"%s\" to complete to \"%s\"",trigger,substitution);
		}
		scrollback_output(current_server,0,output_buffer,TRUE);
	}
}

//the "ping_toggle" command (alternates whether a phrase pings you)
char ping_toggle_command(char *parameters){
	//never ping on empty string
	if(strncmp(parameters,"",BUFFER_SIZE)==0){
		return FALSE;
	}
	
	//phrases are case-insensitive
	char lower_case_parameters[BUFFER_SIZE];
	strncpy(lower_case_parameters,parameters,BUFFER_SIZE);
	strtolower(lower_case_parameters,BUFFER_SIZE);

	char *ping_phrase=NULL;

	//until we find a phrase assume it's new
	int found_ping_idx=-1;
	
	//look through the ping list to see if this phrase was already there
	dlist_entry *ping_phrase_entry=ping_phrases;
	int idx=0;
	while(ping_phrase_entry!=NULL){
		ping_phrase=(char *)(ping_phrase_entry->data);
		if(strncmp(ping_phrase,parameters,BUFFER_SIZE)==0){
			found_ping_idx=idx;
			break;
		}
		
		idx++;
		ping_phrase_entry=ping_phrase_entry->next;
	}
	
	if(found_ping_idx>=0){
		//NOTE: we don't call dlist_free_entry directly because if the 0th item got deleted then our stored pointer needs to be updated
		ping_phrases=dlist_delete_entry(ping_phrases,found_ping_idx,TRUE);
		return FALSE;
	}
	
	
	//if we got here and didn't return, then the phrase was new
	//add it, and return TRUE
	ping_phrase=malloc(sizeof(char)*BUFFER_SIZE);
	strncpy(ping_phrase,lower_case_parameters,BUFFER_SIZE);
	ping_phrases=dlist_append(ping_phrases,ping_phrase);
	return TRUE;
}

//the "sl" client command (moves a server to the left)
void sl_command(){
	//reset scrollback
	scrollback_end=-1;
	
	//if we're currently on a server that isn't the leftmost
	if(current_server>0){
		//go one server left, as intended
		current_server--;
	//at the start, loop back to the end
	//NOTE: if no servers are connected this ends up being -1, as expected :)
	}else{
		current_server=dlist_length(servers)-1;
	}
	
	//if after moving we're on a valid server, clear the output area before the next refresh
	//this conditional guarantees we don't leave it blank for a full refresh/frame (where the user would notice)
	if(current_server>=0){
		wclear(channel_text);
	}
}

//the "sr" client command (moves a server to the right)
void sr_command(){
	//reset scrollback
	scrollback_end=-1;
	
	int server_count=dlist_length(servers);
	
	//if we're currently on a server that isn't the rightmost
	if(current_server<(server_count-1)){
		//go one server to the right, as intended
		current_server++;
	//at the end loop back around
	}else if(server_count>0){
		current_server=0;
	//if there were no servers, then set invalid
	//(as it should have been to start with)
	}else{
		current_server=-1;
	}
	
	//if after moving we're on a valid server, clear the output area before the next refresh
	//this conditional guarantees we don't leave it blank for a full refresh/frame (where the user would notice)
	if(current_server>=0){
		wclear(channel_text);
	}
}

//the "cl" client command (moves a channel to the left)
void cl_command(){
	//reset scrollback
	scrollback_end=-1;
	
	irc_connection *server=get_server(current_server);
	
	int current_channel=server->current_channel;
	//pre-condition for the below loop, else we'd start where we are
	if(current_channel>0){
		current_channel--;
	//at the end loop back around
	}else if(current_channel==0){
		current_channel=dlist_length(server->ch)-1;
	}
	
	server->current_channel=current_channel;
	
	//if after moving we're on a valid channel, clear the output area before the next refresh
	//this conditional guarantees we don't leave it blank for a full refresh/frame (where the user would notice)
	if(current_channel>=0){
		wclear(channel_text);
	}
}

//the "cr" client command (moves a channel to the right)
void cr_command(){
	//reset scrollback
	scrollback_end=-1;

	irc_connection *server=get_server(current_server);
	
	int current_channel=server->current_channel;
	int ch_length=dlist_length(server->ch);
	//pre-condition for the below loop, else we'd start where we are
	if(current_channel<(ch_length-1)){
		current_channel++;
	//at the end loop back around
	}else if(current_channel==(ch_length-1)){
		current_channel=0;
	}
	
	server->current_channel=current_channel;
	
	//if after moving we're on a valid channel, clear the output area before the next refresh
	//this conditional guarantees we don't leave it blank for a full refresh/frame (where the user would notice)
	if(current_channel>=0){
		wclear(channel_text);
	}
}

//the "refresh" command; goes to next unread channel or channel/server
//left->right by default but can go right->left if the argument "left" is given in parameters
//NOTE: this really should be const char *parameters but existing functions use char *parameters so this is for consistency
void refresh_command(char *parameters){
	int refresh_dir=REFRESH_DIRECTION_RIGHT;
	if(strfind("left",parameters)==0){
		refresh_dir=REFRESH_DIRECTION_LEFT;
	}
	
	int server_index=current_server;
	irc_connection *server=get_server(server_index);
	int channel_index=server->current_channel;
	//if you're not connected to a server, this command does nothing
	if((server_index<0) || (channel_index<0)){
		return;
	}

	const int start_server_index=server_index;

#ifdef DEBUG
//	fprintf(error_file,"dbg: refresh_command, refresh_dir=%i, server_index=%i, channel_index=%i\n",refresh_dir,server_index,channel_index);
#endif
	
	//since we're already in the current channel we want to start from the NEXT channel
	//(or previous, depending on our specified direction)
	channel_index+=refresh_dir;
	
	//if this channel is past the end of what's available on the current server
	//then we want to go to the following server
	if(channel_index>=dlist_length(server->ch)){
		channel_index=0;
		server_index+=1;
		if(server_index>=dlist_length(servers)){
			//NOTE: we're safe to assume that there is at least one server and a server_index of 0 is valid
			//because we previously verified that server_index>=0 and channel_index>=0
			server_index=0;
			
			//NOTE: we already set channel_index=0 so we don't need to set it again here; it's 0
		}
	//if this channel is past the end of the list to the left (because we're going in right->left direction)
	}else if(channel_index<0){
		//if there's a previous server, go to the previous server
		if((server_index-1)>=0){
			server_index--;
		//otherwise go to the last server in the list
		//which could potentially be the server we started at
		}else{
			server_index=dlist_length(servers)-1;
		}
		server=get_server(server_index);
		
		//start on the last channel present on whatever server we ended up at
		//because that's the direction we're going
		//NOTE: all servers must always have at least one channel
		channel_index=dlist_length(server->ch)-1;
	}
	//NOTE: if no unread channels are found
	//then we'll just go to the next channel
	//so we're storing the location of that now
	//just in case we get to the end of this function without returning
	int next_server_index=server_index;
	int next_channel_index=channel_index;
	
	//NOTE: channel_index will be re-set to 0 every time we change servers
	//but we don't /necessarily/ want to change the actual global variable yet
	//since we don't know yet whether or not it has unread messages
	//so we need a separate start_channel_index variable here
	int start_channel_index=channel_index;
	
#ifdef DEBUG
//	fprintf(error_file,"dbg: refresh_command, next_server_index=%i, next_channel_index=%i\n",next_server_index,next_channel_index);
#endif
	
	//in the case that we're going in the wrong direction
	//and will hit the first unread thing only after going through everything else first
	//we might end up at the starting server
	//so in that case we need to recheck the start server index
	//and this flag tells us when we're in that case
	char recheck_start_server_index=FALSE;
	
	//while we haven't made a full loop around all servers and channels
	//NOTE: this is a do-while because we may start with other channels on the current server
	do{
		//ensure the server object is always correctly initialized
		server=get_server(server_index);
#ifdef DEBUG
//		fprintf(error_file,"dbg: refresh_command server loop, server_index=%i, recheck_start_server_index=%i\n",server_index,recheck_start_server_index);
#endif
		char in_repeat=FALSE;
		
		//if we were told to recheck the first server again
		//and we're doing so now
		if((recheck_start_server_index==TRUE) && (server_index==start_server_index)){
			//then don't do it again
			recheck_start_server_index=FALSE;
			in_repeat=TRUE;
			
			//and since we are in a repeat case we need to start at the correct end of the list
			//which is the end that we didn't already check the first time
			start_channel_index=(refresh_dir>0)?0:(dlist_length(server->ch)-1);
			channel_index=start_channel_index;
#ifdef DEBUG
//			fprintf(error_file,"dbg: refresh_command server loop start server repeat, server_index=%i, start_channel_index=%i\n",server_index,start_channel_index);
#endif
		}
		
		//while we haven't made a full loop around all channels within this server
		//NOTE: this is a do-while because if we move servers start_channel_index and channel_index will initially be identical
		do {
#ifdef DEBUG
//			fprintf(error_file,"dbg: refresh_command channel loop, channel_index=%i, start_channel_index=%i\n",channel_index,start_channel_index);
#endif
			channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,channel_index)->data);
			//if there was any new content in this channel
			if((ch!=NULL) && (ch->new_content)){
				//then that's now the current channel!
				//set global vars and return
				current_server=server_index;
				server->current_channel=channel_index;
				
				//NOTE: we do not need to refresh any output here
				//because the event_poll code, upon seeing that the current_server and/or current_channel changed
				//will know to do that automatically
				return;
			}
			
			//move channel in the specified direction
			channel_index+=refresh_dir;
			
			//if we're past the end of the channel list to the right
			//then start over from the left (index 0)
			if(channel_index>=dlist_length(server->ch)){
				channel_index=0;
				//if this is the server we started on and we're going in the left->right direction
				if((server_index==start_server_index) && (refresh_dir>0)){
					//and this isn't our second time checking this server
					if(in_repeat==FALSE){
						//then exit this loop
						//NOTE: after control exits this loop server_index will be updated
						//and channel_index will get reset to the last channel on that server
						//so no other action is needed here
#ifdef DEBUG
//						fprintf(error_file,"dbg: refresh_command channel loop, setting flag to come back to this server later (left->right)\n");
#endif
						
						recheck_start_server_index=TRUE;
						break;
					}
				}
			//if we're past the end of the channel list to the left
			//then start over from the right (index length-1)
			}else if(channel_index<0){
				channel_index=dlist_length(server->ch)-1;
				//if this is the server we started on and we're going in the right->left direction
				if((server_index==start_server_index) && (refresh_dir<0)){
					//and this isn't our second time checking this server
					if(in_repeat==FALSE){
						//then exit this loop
						//NOTE: after control exits this loop server_index will be updated
						//and channel_index will get reset to the last channel on that server
						//so no other action is needed here

#ifdef DEBUG
//						fprintf(error_file,"dbg: refresh_command channel loop, setting flag to come back to this server later (right->left)\n");
#endif

						recheck_start_server_index=TRUE;
						break;
					}
				}
			}
		} while(channel_index!=start_channel_index);
		
		//move server in the specified direction
		server_index+=refresh_dir;
		
		//if we're past the end of the server list to the right
		//then start over from the left (index 0)
		if(server_index>=dlist_length(servers)){
			server_index=0;
		//if we're past the end of the server list to the left
		//then start over from the right (index length-1)
		}else if(server_index<0){
			server_index=dlist_length(servers)-1;
		}
		
		//if we're going left->right when we go to the next server we'll start at channel 0
		//if we're going right->left then when we go to the next server (previous server?) we'll start at the end of the channel list
		server=get_server(server_index);
		start_channel_index=(refresh_dir>0)?0:(dlist_length(server->ch)-1);
		channel_index=start_channel_index;
	//while we're not on the server we started on
	//or we are on that server but we're supposed to be because we're rechecking it
	}while((server_index!=start_server_index) || (recheck_start_server_index==TRUE));

	//if we got here and didn't return then just go to the next channel/server
	//in the list relative to wherever we started
	current_server=next_server_index;
	server=get_server(next_server_index);
	server->current_channel=next_channel_index;
}

//the "log" client command (enables logging where possible)
void log_command(){
	irc_connection *server=get_server(current_server);
	
	if(server->keep_logs==FALSE){
		//if we have the ability to make logs
		if(can_log){
			server->keep_logs=TRUE;
			scrollback_output(current_server,0,"accirc: keep_logs set to TRUE (opening log files)",TRUE);
			
			//open any log files we may need
			//look through the channels
			int channel_index;
			dlist_entry *ch_entry=server->ch;
			for(channel_index=0;ch_entry!=NULL;channel_index++){
				//TODO: use a subdirectory for the user's nick so that multiple logs can be kept for simultaneous connections with different nicks
				//the directory structure should then be ~/.local/share/accirc/logs/<server>/<nick>/ with all the #channel files under that
				//NOTE: this is complicated because the user's nick can change ove time so for now that's just not being done
				
				//try to open a file for every channel
				channel_info *ch=(channel_info *)(ch_entry->data);
				
				char file_location[BUFFER_SIZE];
				snprintf(file_location,BUFFER_SIZE,"%s/.local/share/accirc/%s/%s/%s",getenv("HOME"),LOGGING_DIRECTORY,server->server_name,ch->name);
				if(verify_or_make_parent_dirs_for(file_location)){
					//note if this fails it will be set to NULL and hence will be skipped over when trying to output to it
					ch->log_file=fopen(file_location,"a");
					
					
					if(ch->log_file!=NULL){
						//turn off buffering since I need may this output immediately and buffers annoy me for that
						setvbuf(ch->log_file,NULL,_IONBF,0);
					}
				}else{
					char error_buffer[BUFFER_SIZE];
					snprintf(error_buffer,BUFFER_SIZE,"accirc: Err: could not open log file for channel %s!",ch->name);
					scrollback_output(current_server,channel_index,error_buffer,TRUE);
				}
				ch_entry=ch_entry->next;
			}
		}else{
			scrollback_output(current_server,0,"accirc: Err: your environment doesn't support logging, cannot set it!",TRUE);
		}
	}else{
		scrollback_output(current_server,0,"accirc: Err: keep_logs already set to TRUE, no changes made",TRUE);
	}
}

//the "no_log" client command (disables logging)
void no_log_command(){
	irc_connection *server=get_server(current_server);
	
	if(server->keep_logs==TRUE){
		server->keep_logs=FALSE;
		scrollback_output(current_server,0,"accirc: keep_logs set to FALSE (closing log files)",TRUE);
		
		//close any open logs we were writing to
		dlist_entry *ch_entry=server->ch;
		while(ch_entry!=NULL){
			channel_info *ch=(channel_info *)(ch_entry->data);
			if(ch->log_file!=NULL){
				fclose(ch->log_file);
				
				//reset the structure to hold NULL
				ch->log_file=NULL;
			}
			ch_entry=ch_entry->next;
		}
	}else{
		scrollback_output(current_server,0,"accirc: Err: keep_logs already set to FALSE, no changes made",TRUE);
	}
}

//the "rsearch" client command (searches from bottom to top for a string)
void rsearch_command(char *input_buffer, char *command, char *parameters, int old_scrollback_end){
	irc_connection *server=get_server(current_server);
	
	if(!strcmp(parameters,"")){
		//too few arguments given, do nothing (give up)
		
		//tell the user there were too few arguments
		char error_buffer[BUFFER_SIZE];
		snprintf(error_buffer,BUFFER_SIZE,"accirc: Err: too few arguments given to \"%s\"",command);
		
		//use the ncurses UI to notify the user
		//NOTE: the rsearch command is only called after a check for current_server being valid from within parse_input
		//this means we can depend on current server being usable at this point
		//(and the current channel is always valid, no channels connected goes to raw server output area)
		scrollback_output(current_server,server->current_channel,error_buffer,TRUE);
	}else{
		//the entire scrollback for the current channel
		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
		char **channel_scrollback=ch->content;
		
		//the line to start the reverse search at
		int search_line=0;
		//if the user is already viewing history start from just above where they are viewing
		if(old_scrollback_end>=0){
			search_line=old_scrollback_end-2;
		}else{
			//if they are not viewing history start from the end
			int n;
			for(n=0;n<MAX_SCROLLBACK;n++){
				if(channel_scrollback[n]!=NULL){
					search_line++;
				}
			}
		}
		
		//search for the given parameters in every line from the end up until either they are found or we run out of lines
		while(search_line>=0){
			//(the null check here is defensive, since there are nulls in the structure, but none of the lines we're checking should be null)
			if((channel_scrollback[search_line]!=NULL) && (strfind(parameters,channel_scrollback[search_line])>=0)){
				break;
			}
			
			search_line--;
		}
		
		//if we didn't run out of lines, we found the string in question, set the user's display to that and update
		if(search_line>=0){
			//the +1 here is because scrollback_end is not inclusive (output loops with a  a "<scrollback_end" condition)
			scrollback_end=(search_line+1);
			refresh_channel_text();
		//display an error if the term was not found
		}else{
			char error_buffer[BUFFER_SIZE];
			snprintf(error_buffer,BUFFER_SIZE,"accirc: Could not find search term \"%s\"",parameters);
			scrollback_output(current_server,server->current_channel,error_buffer,TRUE);
		}
	}
}

//the "up" client command (scroll back in the current channel)
//TODO: compute correct stop scrolling bound in this case
void up_command(char *input_buffer, char *command, char *parameters, int old_scrollback_end){
	//start at wherever the user was
	scrollback_end=old_scrollback_end;
	
	//if we are connected to a server
	if(current_server>=0){
		irc_connection *server=get_server(current_server);
		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
		
		int line_count;
		char **scrollback=ch->content;
		for(line_count=0;(line_count<MAX_SCROLLBACK)&&(scrollback[line_count]!=NULL);line_count++);
		
		//if there is more text than area to display allow it scrollback (else don't)
//		if(line_count>height-RESERVED_LINES){
		if(line_count>0){
			//if we're already scrolled back and we can go further
//			if((scrollback_end-height+RESERVED_LINES)>0){
			if(scrollback_end>1){
				scrollback_end--;
			//if we're not scrolled back start now
			}else if(scrollback_end<0){
				int n;
				//note after this loop n will be one line AFTER the last valid line of scrollback
				for(n=0;(n<MAX_SCROLLBACK)&&(scrollback[n]!=NULL);n++);
				//so subtract one
				n--;
				//if there is scrollback to view
				if(n>=0){
					scrollback_end=n;
				}
			}
		}
		refresh_channel_text();
	}
}

//the "down" client command (scroll forward in the current channel)
void down_command(char *input_buffer, char *command, char *parameters, int old_scrollback_end){
	scrollback_end=old_scrollback_end;
	
	//if we are connected to a server
	if(current_server>=0){
		irc_connection *server=get_server(current_server);
		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
		
		char **scrollback=ch->content;
		//if we're already scrolled back and there is valid scrollback below this
		if((scrollback_end>=0)&&(scrollback_end<(MAX_SCROLLBACK-1))&&(scrollback[scrollback_end+1]!=NULL)){
			scrollback_end++;
		//if we're out of scrollback to view, re-set and make this display new data as it gets here
		}else if(scrollback_end>=0){
			scrollback_end=-1;
		}
		refresh_channel_text();
	}
}

//the "head" client command (scrolls to top of scrollback area)
void head_command(){
	irc_connection *server=get_server(current_server);
	if(server==NULL){
		return;
	}
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
	
	char **channel_scrollback=ch->content;
	int line_count=0;
	int n;
	for(n=0;n<MAX_SCROLLBACK;n++){
		if(channel_scrollback[n]!=NULL){
			line_count++;
		}
	}
	
	//if there are any lines at all, go to the first one
	if(line_count>0){
		scrollback_end=1;
	//otherwise go to the last (there are none, so really there is no "last", but you know)
	}else{
		scrollback_end=-1;
	}
	
	refresh_channel_text();
}

//the "tail" client command (scrolls to bottom of scrollback area)
void tail_command(){
	//NOTE: scrollback_end got set to -1 at the start of parse_input so tail just needs to update the display
	refresh_channel_text();
}

//"join" a PM conversation
void hi_command(char *input_buffer, char *command, char *parameters){
	char output_buffer[BUFFER_SIZE];
	int output_channel;
	
	//new faux channel name
	char pm_ch_name[BUFFER_SIZE];
	strncpy(pm_ch_name,parameters,BUFFER_SIZE);
	
	//the message to send on open, if one is given
	char message[BUFFER_SIZE];
	strncpy(message,"",BUFFER_SIZE);
	
	//set some sane default output just in case it doesn't change
	output_channel=0;
	strncpy(output_buffer,"",BUFFER_SIZE);
	
	//check for space in parameters, and if there is a space
	//the consider the pre-space content to be the channel/user name
	//and the post-space content to be a message to send to that user once the faux channel is opened
	int first_space=strfind(" ",parameters);
	if(first_space>=0){
		substr(pm_ch_name,parameters,0,first_space);
		substr(message,parameters,first_space+1,strlen(parameters)-(first_space+1));
	}
	
	join_new_channel(current_server,pm_ch_name,output_buffer,&output_channel,TRUE);
	scrollback_output(current_server,output_channel,output_buffer,TRUE);
	
	if(strlen(message)>0){
		//NOTE: we don't need to swap servers or channels
		//because we know that we are joining a channel on the current_server
		//and because we know join_new_channel sets the output_channel to be the newly-created channel
		//so this is always the correct server and channel to output to
		//by the time control gets to this statement
		privmsg_command(message);
	}
}

//TODO: add support for "otr_hi", which would initialize an otr PM chat via libotr
//when compilation is enabled for this feature

//"part" a PM conversation
void bye_command(char *input_buffer, char *command, char *parameters){
	irc_connection *server=get_server(current_server);
	
	int channel_index=server->current_channel;
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,channel_index)->data);
	
	char output_buffer[BUFFER_SIZE];
	
	//ensure this is a PM channel and not a real channel, don't want to /bye those, you gotta part like a good person
	if(ch->is_pm){
		snprintf(output_buffer,BUFFER_SIZE,"accirc: Parting (faux) PM channel \"%s\"",ch->name);
		
		leave_channel(current_server,ch->name);
	}else{
		strncpy(output_buffer,"accirc: Err: channel you tried to \"bye\" is not a PM!",BUFFER_SIZE);
	}
	scrollback_output(current_server,0,output_buffer,TRUE);
}

//privmsg from user input (treated as a pseudo-command)
void privmsg_command(char *input_buffer){
	if(current_server>=0){
		irc_connection *server=get_server(current_server);
		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
		
		//if we're on the server channel treat it as a command (recurse)
		if(server->current_channel==0){
			char tmp_buffer[BUFFER_SIZE];
			snprintf(tmp_buffer,BUFFER_SIZE,"%c%s",server_escape,input_buffer);
			//but don't keep history for this recursion call
			parse_input(tmp_buffer,FALSE);
		}else{
			//format the text for the server's benefit
			char output_buffer[BUFFER_SIZE];
			snprintf(output_buffer,BUFFER_SIZE,"PRIVMSG %s :%s\n",ch->name,input_buffer);
			server_write(current_server,output_buffer);
			
			//then format the text for my viewing benefit (this is also what will go in logs, with a newline)
			//accounting specially for if the user sent a CTCP ACTION
			char ctcp[BUFFER_SIZE];
			snprintf(ctcp,BUFFER_SIZE,"%cACTION ",0x01);
			if(strfind(ctcp,input_buffer)==0){
				char tmp_buffer[BUFFER_SIZE];
				substr(tmp_buffer,input_buffer,strlen(ctcp),strlen(input_buffer)-strlen(ctcp)-1);
				snprintf(output_buffer,BUFFER_SIZE,">> *%s %s",server->nick,tmp_buffer);
			}else{
				char nick_mode_str[BUFFER_SIZE];
				strncpy(nick_mode_str,"",BUFFER_SIZE);
				
				//display channel modes with the nick if possible
				int nick_ch_idx=nick_idx(ch,server->nick,0);
				if((nick_ch_idx>=0) && (server->use_mode_str)){
					dlist_entry *nick_entry=dlist_get_entry(ch->user_names,nick_ch_idx);
					nick_info *nick_content=(nick_info*)(nick_entry->data);
					snprintf(nick_mode_str,BUFFER_SIZE,"%c",get_mode_prefix(nick_content->mode_str));
				}
				
				snprintf(output_buffer,BUFFER_SIZE,">> <%s%s> %s",nick_mode_str,server->nick,input_buffer);
			}
			
			//place my own text in the scrollback for this server and channel
			scrollback_output(current_server,server->current_channel,output_buffer,TRUE);
		}
	}else{
#ifdef DEBUG
//		int foreground,background;
//		sscanf(input_buffer,"%i %i",&foreground,&background);
//		wblank(channel_text,width,height-RESERVED_LINES);
//		wcoloron(channel_text,foreground,background);
//		wprintw(channel_text,"This is a sample string in fg=%i bg=%i",foreground,background);
//		wcoloroff(channel_text,foreground,background);
//		wrefresh(channel_text);
#endif
	}
}

//check for aliased commands, if one is found do the substitution and feed it back into parse_input without using history
//returns TRUE if an alised substitution was made, else FALSE
char handle_aliased_command(char *command, char *parameters){
	dlist_entry *alias_list_entry=alias_list;
	while(alias_list_entry!=NULL){
		alias *alias_item=(alias *)(alias_list_entry->data);
		
		//NOTE: aliases are not case-sensitive; this is intentional
		//if a command is found to match, do the substitution and parse it again
		if(!strncmp(alias_item->trigger,command,BUFFER_SIZE)){
			char new_command_buffer[BUFFER_SIZE];
			snprintf(new_command_buffer,BUFFER_SIZE,"%s %s",alias_item->substitution,parameters);
			parse_input(new_command_buffer,FALSE);
			
			return TRUE;
		}
		
		alias_list_entry=alias_list_entry->next;
	}
	
	//if we got here and didn't handle a command, there was no alias for that
	return FALSE;
}

//swap channel with a nearby channel
//the delta determines the direction to move in
void swap_channel(int server_idx, int delta){
	irc_connection *server=get_server(server_idx);
	
	int ch_count=dlist_length(server->ch);
	
	//if there are fewer than two channels, then swapping doesn't do anything
	//so this is a no-op
	if(ch_count<2){
		return;
	}
	
	int cur_idx=server->current_channel;
	//you can't swap the server channel so just give up if the user tried
	if(cur_idx==0){
		scrollback_output(current_server,0,"accirc: Warn: Ignoring swap_channel call because the SERVER view is reserved...",TRUE);
		return;
	}
	
	//find the index to swap with based on the delta
	int swap_idx=cur_idx+delta;
	
	//if we hit either end, wrap around
	//keeping in mind that index 0, the server index, is reserved
	if(swap_idx<1){
		swap_idx=ch_count-1;
	}else if(swap_idx>(ch_count-1)){
		swap_idx=1;
	}
	if(cur_idx==swap_idx){
		scrollback_output(current_server,0,"accirc: Warn: Ignoring swap_channel call because it would swap a channel with itself, which is meaningless...",TRUE);
		return;
	}
	
	channel_info *cur_ch=(channel_info *)(dlist_get_entry(server->ch,cur_idx)->data);
	channel_info *swap_ch=(channel_info *)(dlist_get_entry(server->ch,swap_idx)->data);
	
	server->ch=dlist_swap(server->ch,cur_idx,swap_idx);
	
	//let the user know we swapped channels
	char notify_buffer[BUFFER_SIZE];
	snprintf(notify_buffer,BUFFER_SIZE,"accirc: Swapped channel %s and channel %s in channel list",cur_ch->name,swap_ch->name);
	scrollback_output(server_idx,0,notify_buffer,TRUE);
	
	//set the swap point to be the current channel
	//since at the time of command issue it was the current channel
	server->current_channel=swap_idx;
	
	//and lastly update the channel list
	refresh_channel_list();
}

//swap server with a nearby server
//the delta determines the direction to move in
void swap_server(int server_idx, int delta){
	int srvr_count=dlist_length(servers);
	
	//if there are fewer than two channels, then swapping doesn't do anything
	//so this is a no-op
	if(srvr_count<2){
		return;
	}
	
	int cur_idx=server_idx;
	
	//find the index to swap with based on the delta
	int swap_idx=cur_idx+delta;
	
	//if we hit either end, wrap around
	if(swap_idx<0){
		swap_idx=srvr_count-1;
	}else if(swap_idx>(srvr_count-1)){
		swap_idx=0;
	}
	if(cur_idx==swap_idx){
		scrollback_output(current_server,0,"accirc: Warn: Ignoring swap_server call because it would swap a server with itself, which is meaningless...",TRUE);
		return;
	}
	
	irc_connection *cur_srvr=get_server(cur_idx);
	irc_connection *swap_srvr=get_server(swap_idx);
	
	servers=dlist_swap(servers,cur_idx,swap_idx);

	//set the swap point to be the current server
	//since at the time of command issue it was the current server
	current_server=swap_idx;
	
	//let the user know we swapped servers
	char notify_buffer[BUFFER_SIZE];
	snprintf(notify_buffer,BUFFER_SIZE,"accirc: Swapped server %s and server %s in server list",cur_srvr->server_name,swap_srvr->server_name);
	scrollback_output(current_server,0,notify_buffer,TRUE);
	
	//and lastly update the channel list
	refresh_channel_list();
}

//the "set_channel_order" command; sets the order of channels in the server
//NOTE: this does NOT apply to the SERVER channel which is always at index 0 and is needed for some types of output
void set_channel_order_command(char *parameters){
	char orig_params[BUFFER_SIZE];
	strncpy(orig_params,parameters,BUFFER_SIZE);
	
	irc_connection *server=get_server(current_server);
	
	//NOTE: set_channel_order is not cumulative
	//if there is an existing channel_order we're over-writing it
	//so clear out anything existing
	dlist_free(server->channel_order,TRUE);
	server->channel_order=NULL;
	
	//split by spaces (or by first space iteratively)
	//and save each channel name in the list in server->channel_order
	while(strnlen(parameters,BUFFER_SIZE)>0){
		char *channel_name=(char *)(malloc(BUFFER_SIZE*sizeof(char)));
		strncpy(channel_name,"",BUFFER_SIZE);
		
		//channels are space-separated
		int space_idx=strfind(" ",parameters);
		//if there's more than one channel present
		if(space_idx>=0){
			//channel_name is the first one in the remaining parameters
			substr(channel_name,parameters,0,space_idx);
			//and then we shift parameters over to get the remaining channel names
			substr(parameters,parameters,space_idx+1,strnlen(parameters,BUFFER_SIZE)-(space_idx+1));
		//if this is the only channel present
		}else{
			//then the whole parameters string is the channel_name
			//and we want to set parameters to empty string to break the loop
			strncpy(channel_name,parameters,BUFFER_SIZE);
			strncpy(parameters,"",BUFFER_SIZE);
		}
		
		server->channel_order=dlist_append(server->channel_order,channel_name);
	}
	
	//remember the name of the channel we had previously selected
	//so we can make it active again after any re-ordering is applied
	char current_ch_name[BUFFER_SIZE];
	strncpy(current_ch_name,"",BUFFER_SIZE);
	if(server->current_channel>0){
		strncpy(current_ch_name,(const char *)(((channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data))->name),BUFFER_SIZE);
	}
	
	//in the case that the channels in question are ALREADY present in the server connection
	//apply ordering to the existing items
	//and put anything whose order was not specified at the end
	//NOTE: we do NOT apply any ordering to the SERVER channel (at index 0)
	for(int ch_idx=0;ch_idx<dlist_length(server->ch);ch_idx++){
		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,ch_idx)->data);
		
		int new_ch_idx=apply_channel_order(server,ch->name);
	}
	
	//re-select the previously-selected current channel, if there was one
	if(strnlen(current_ch_name,BUFFER_SIZE)>0){
		int current_channel=ch_idx_from_name(server,current_ch_name);
		if(current_channel>=0){
			server->current_channel=current_channel;
		}
	}else if(dlist_length(server->ch)>0){
		server->current_channel=0;
	}
	
	//let the user know we saved channel order
	char notify_buffer[BUFFER_SIZE];
	snprintf(notify_buffer,BUFFER_SIZE,"accirc: Set channel order for server %s to be \"%s\" (will be applied during JOINs)",server->server_name,orig_params);
	scrollback_output(current_server,0,notify_buffer,TRUE);
}

//END parse_input HELPER FUNCTIONS

//TODO: refactor this, as this function is currently many hundreds of lines long!
//parse user's input (note this is conextual based on current server and channel)
//because some input may be given recursively or from key bindings, there is a history flag to tell if we should actually consider this input in the history
void parse_input(char *input_buffer, char keep_history){
	//store the old scrollback for rsearch
	int old_scrollback_end=scrollback_end;
	
	//go to the end of scrollback because why would the user input something and not want to see it?
	scrollback_end=-1;
	
	//ignore blank commands
	if(!strcmp("",input_buffer)){
		strncpy(input_buffer,"\0",BUFFER_SIZE);
		return;
	}
	
	if(keep_history){
		add_history_entry(input_buffer);
	}
	
	//flags to tell if this is any kind of command
	char server_command=FALSE;
	char client_command=FALSE;
	
	//note that the client and server command escapes are settings
	if((input_buffer[0]==server_escape)||(input_buffer[0]==client_escape)||(input_buffer[0]=='\\')){
		//server command
		if(input_buffer[0]==server_escape){
			server_command=TRUE;
			client_command=FALSE;
		//client command
		}else if(input_buffer[0]==client_escape){
			server_command=FALSE;
			client_command=TRUE;
		//escape
		}else if(input_buffer[0]=='\\'){
			server_command=FALSE;
			client_command=FALSE;
		}
		
		//trim off the first character
		char tmp_buffer[BUFFER_SIZE];
		substr(tmp_buffer,input_buffer,1,strlen(input_buffer)-1);
		strncpy(input_buffer,tmp_buffer,BUFFER_SIZE);
	}
	//if the user is not connected to any server and didn't issue a known client or server command
	//then assume it's a client command; they probably want /help
	if((current_server<0) && (!server_command) && (!client_command)){
		client_command=TRUE;
	//if the lines are intended to be delayed then delay them
	}else if((client_command || server_command) && (current_server>=0) && post_listen && (!keep_history)){
		irc_connection *server=get_server(current_server);
		
		//NOTE: this additional handling is necessary
		//because client commands are now accepted as post- commands
		//and so if we don't include any type of way to get out of the post- handling
		//then you can only connect to one irc server in the rc file
		//but we want to allow multiple irc server connections to be specified
		if(client_command && (strfind("no_post",input_buffer)==0)){
			post_listen=FALSE;
			scrollback_output(current_server,0,"accirc: no longer listening for post- commands",TRUE);
		}else{
			//NOTE: we used a linked list so there is no maximum number of commands we can store
			
			//append this command to the current server's post_commands
			char *post_cmd_buffer=malloc(sizeof(char)*(BUFFER_SIZE));
			snprintf(post_cmd_buffer,(sizeof(char)*(BUFFER_SIZE)),"%c%s",server_escape,input_buffer);
			if(client_command){
				snprintf(post_cmd_buffer,(sizeof(char)*(BUFFER_SIZE)),"%c%s",client_escape,input_buffer);
			}
			server->post_commands=dlist_append(server->post_commands,post_cmd_buffer);
			
			//let the user know we did something
			char notify_buffer[BUFFER_SIZE];
			snprintf(notify_buffer,(sizeof(char)*BUFFER_SIZE),"accirc: Saving post-%s command \"%s\" for later",server->post_type,post_cmd_buffer);
			scrollback_output(current_server,0,notify_buffer,TRUE);
		}
		
		return;
	//the user manually entered /post into the client, rather than putting it in an rc file
	}else if((client_command || server_command) && (current_server>=0) && post_listen){
		char notify_buffer[BUFFER_SIZE];
		strncpy(notify_buffer,"accirc: post_listen flag ignored (keep_history is true) (/post commands should only exist in an rc file!); use /no_post to stop seeing this",BUFFER_SIZE);
		scrollback_output(current_server,0,notify_buffer,TRUE);
	//NOTE: post-message delays are intentionally mutually exclusive with this statement
	//since depending on race conditions it's possible for messages to leak to channels otherwise
	//and we don't want anything like a /msg NickServ passwork leaking to the channel text
	}else if((current_server>=0) && post_listen){
		irc_connection *server=get_server(current_server);
		char notify_buffer[BUFFER_SIZE];
		snprintf(notify_buffer,sizeof(char)*BUFFER_SIZE,"accirc: Warn: rc file error, the line \"%s\" is ignored from post-%s commands for security reasons because it does not start with %c (server command) or %c (client command)",input_buffer,server->post_type,server_escape,client_escape);
		scrollback_output(current_server,0,notify_buffer,TRUE);
		return;
	}
	
	irc_connection *server=get_server(current_server);
	
	//if it's a client command handle that here
	if(client_command){
		char command[BUFFER_SIZE];
		char parameters[BUFFER_SIZE];
		int first_space=strfind(" ",input_buffer);
		
		//space not found, command with no parameters
		if(first_space<0){
			strncpy(command,input_buffer,BUFFER_SIZE);
			strncpy(parameters,"",BUFFER_SIZE);
		}else{
			substr(command,input_buffer,0,first_space);
			substr(parameters,input_buffer,first_space+1,strlen(input_buffer)-(first_space+1));
		}
		
		//the good stuff, the heart of command handling :)
		
		//this set of commands does not depend on being connected to a server
		
		if(!strcmp("help",command)){
			//TODO: and support for a /help argument so things like /help <command> will output what you'd expect
			//and make the generic /help output a list of help commands on a single line rather than a whole screenful of output
			//also, include full information on all key bindings in the built-in help
			//(all this documentation already exists in the man page accirc.man, it's just not currently included in the program itself)
			
			if(current_server>=0){
				char notify_buffer[BUFFER_SIZE];
				strncpy(notify_buffer,"accirc: For detailed help and additional commands please read the manual page",BUFFER_SIZE);
				scrollback_output(current_server,0,notify_buffer,TRUE);
				
				int n;
				for(n=0;n<(sizeof(command_list)/sizeof(command_list[0]));n++){
					snprintf(notify_buffer,BUFFER_SIZE,"accirc: command: %s",command_list[n]);
					scrollback_output(current_server,0,notify_buffer,TRUE);
				}
			}else{
				wblank(channel_text,width,height-RESERVED_LINES);
				wmove(channel_text,0,0);
				wprintw(channel_text,"accirc: For detailed help please read the manual page, the /connect command is probably what you're looking for if you're reading this");
				
				char notify_buffer[BUFFER_SIZE];
				int n;
				for(n=0;n<(sizeof(command_list)/sizeof(command_list[0]));n++){
					if((n+2)<(height-RESERVED_LINES)){
						snprintf(notify_buffer,BUFFER_SIZE,"accirc: command: %s",command_list[n]);
						wmove(channel_text,(n+1),0);
						wprintw(channel_text,notify_buffer,TRUE);
					}else if((n+1)<(height-RESERVED_LINES)){
						wmove(channel_text,(n+1),0);
						wprintw(channel_text,"accirc: Warn: help cut off because your terminal is too small!",TRUE);
					}
				}
				
				wrefresh(channel_text);
			}
		//connect to a server
		}else if(!strcmp("connect",command)){
			connect_command(input_buffer,command,parameters,FALSE);
		//connect to a server with encryption
		}else if(!strcmp("sconnect",command)){
#ifdef _OPENSSL
			connect_command(input_buffer,command,parameters,TRUE);
#else
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: sconnect can't work, this was compiled without SSL support!",TRUE);
			}else{
				wblank(channel_text,width,height-RESERVED_LINES);
				wmove(channel_text,0,0);
				wprintw(channel_text,"accirc: sconnect can't work, this was compiled without SSL support!");
				wrefresh(channel_text);
			}
#endif
		}else if(!strcmp(command,"exit")){
			exit_command(input_buffer,command,parameters);
		//usleep command
		}else if(!strcmp(command,"usleep")){
			scrollback_output(current_server,0,"accirc: usleeping...",TRUE);
			//sleep as requested (in milliseconds)
			usleep(atoi(parameters));
		//comment command (primarily for the .rc file)
		}else if(!strcmp(command,"comment")){
			//ignore it
		
		//reset the escape character for client
		}else if(!strcmp(command,"cli_escape")){
			cli_escape_command(input_buffer,command,parameters);
		//reset the escape character for server
		}else if(!strcmp(command,"ser_escape")){
			ser_escape_command(input_buffer,command,parameters);
		//register a new alias that will work for the remainder of this session
		}else if(!strcmp(command,"alias")){
			alias_command(input_buffer,command,parameters);
		//change the time format to output
		}else if(!strcmp(command,"time_format")){
			if(strlen(parameters)>0){
				strncpy(time_format,parameters,BUFFER_SIZE);
			}
			
			if(current_server>=0){
				char notify_buffer[BUFFER_SIZE];
				snprintf(notify_buffer,BUFFER_SIZE,"accirc: updated time format to \"%s\"",parameters);
				scrollback_output(current_server,0,notify_buffer,TRUE);
			}
		//change the version response to a custom one, if given
		}else if(!strcmp(command,"set_version")){
			if(strlen(parameters)>0){
				strncpy(custom_version,parameters,BUFFER_SIZE);
			}else{
				snprintf(custom_version,BUFFER_SIZE,"accidental_irc v%s compiled %s %s",VERSION,__DATE__,__TIME__);
			}
			
			if(current_server>=0){
				char notify_buffer[BUFFER_SIZE];
				snprintf(notify_buffer,BUFFER_SIZE,"accirc: updated custom version string to \"%s\"",custom_version);
				scrollback_output(current_server,0,notify_buffer,TRUE);
			}
		//toggle easy mode on
		}else if(!strcmp(command,"easy_mode")){
			easy_mode=TRUE;
			
			char easy_mode_buf[BUFFER_SIZE];
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias nick %cnick",client_escape,server_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias join %cjoin",client_escape,server_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias part %cpart",client_escape,server_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias quit %cexit",client_escape,client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias msg %chi",client_escape,client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%ctime_format %%Y-%%m-%%d %%R:%%S",client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: easy_mode turned ON",TRUE);
			}
		//toggle easy mode off
		}else if(!strcmp(command,"no_easy_mode")){
			easy_mode=FALSE;

			char easy_mode_buf[BUFFER_SIZE];
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias nick ",client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias join ",client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias part ",client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias quit ",client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%calias msg ",client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			snprintf(easy_mode_buf,BUFFER_SIZE,"%ctime_format %%s",client_escape);
			parse_input(easy_mode_buf,FALSE);
			
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: easy_mode turned OFF",TRUE);
			}
		}else if(!strcmp(command,"center_server_list")){
			center_server_list=TRUE;
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: will now always use small screen server list display",TRUE);
			}
		}else if(!strcmp(command,"no_center_server_list")){
			center_server_list=FALSE;
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: will now use responsive screen server list display",TRUE);
			}
		//toggle a phrase in the ping phrases list
		}else if(!strcmp(command,"ping_toggle")){
			char ping_state=ping_toggle_command(parameters);
			if(current_server>=0){
				char output_buffer[BUFFER_SIZE];
				snprintf(output_buffer,BUFFER_SIZE,"accirc: will now %s on phrase %s",(ping_state==TRUE)?"PING":"NOT PING",parameters);
				scrollback_output(current_server,0,output_buffer,TRUE);
			}
		}else if(!strcmp(command,"auto_hi")){
			auto_hi=TRUE;
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: will now automatically create faux channel on new PM",TRUE);
			}
		}else if(!strcmp(command,"no_auto_hi")){
			auto_hi=FALSE;
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: will now NOT automatically create faux channel on new PM",TRUE);
			}
#ifdef _OPENSSL
		}else if(!strcmp(command,"ssl_cert_check")){
			ssl_cert_check=TRUE;
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: ssl_cert_check set to TRUE",TRUE);
			}
		}else if(!strcmp(command,"no_ssl_cert_check")){
			ssl_cert_check=FALSE;
			if(current_server>=0){
				scrollback_output(current_server,0,"accirc: ssl_cert_check set to FALSE",TRUE);
			}
#endif
		//this set of command depends on being connected to a server, so first check that we are
		}else if((server!=NULL) && (current_server>=0)){
			//TODO: update helper functions to take the server object instead of or in addition to current_server where appropriate
			
			//move a server to the left
			if(!strcmp(command,"sl")){
				sl_command();
			//move a server to the right
			}else if(!strcmp(command,"sr")){
				sr_command();
			//move a channel to the left
			}else if(!strcmp(command,"cl")){
				cl_command();
			//move a channel to the right
			}else if(!strcmp(command,"cr")){
				cr_command();
			//refresh (right by default or when "right" is given, but left if argument "left" is given)
			//also known as a "cycle to next unread channel or server" command (key bindings f5 and f6)
			//also known as "next unread left" and "next unread right" (a user can choose the direction)
			//this is to make it easier to go to just the channels with messages when cycling through
			}else if(!strcmp(command,"refresh")){
				refresh_command(parameters);
			//CTCP ACTION, bound to the common "/me"
			}else if(!strcmp(command,"me")){
				//attach the control data and recurse
				char tmp_buffer[BUFFER_SIZE];
				snprintf(tmp_buffer,BUFFER_SIZE,"%cACTION %s%c",0x01,parameters,0x01);
				//don't keep that in the history though
				parse_input(tmp_buffer,FALSE);
			//r is short for "reply"; this will send a PM to the last user we got a PM from
			}else if(!strcmp(command,"r")){
				//prepend the "privmsg <nick> :" and recurse
				char tmp_buffer[BUFFER_SIZE];
				snprintf(tmp_buffer,BUFFER_SIZE,"%cprivmsg %s :%s",server_escape,server->last_pm_user,parameters);
				//don't keep the recursion in the history, if the user wants it they can get the /r command out of history
				parse_input(tmp_buffer,FALSE);
			//reverse, an easter egg to flip text around
			}else if(!strcmp(command,"reverse")){
				//flip the text around and recurse
				char tmp_buffer[BUFFER_SIZE];
				snprintf(tmp_buffer,BUFFER_SIZE,"%s",parameters);
				//reverse!
				strnrev(tmp_buffer);
				
				//don't keep the recursion in the history
				parse_input(tmp_buffer,FALSE);
			//morse encode function
			}else if(!strcmp(command,"morse")){
				char tmp_buffer[BUFFER_SIZE];
				snprintf(tmp_buffer,BUFFER_SIZE,"%s",parameters);
				//encode the text in morse
				morse_encode(parameters,tmp_buffer,BUFFER_SIZE);
				
				//don't keep the recursion in the history
				parse_input(tmp_buffer,FALSE);
			//morse decode function
			}else if(!strcmp(command,"unmorse")){
				char tmp_buffer[BUFFER_SIZE];
				snprintf(tmp_buffer,BUFFER_SIZE,"%s",parameters);
				//decode the text in morse
				morse_decode(parameters,tmp_buffer);
				
				//don't keep the recursion in the history
				parse_input(tmp_buffer,FALSE);
			}else if(!strcmp(command,"set_channel_order")){
				set_channel_order_command(parameters);
			//hide user mode changes on this server
			}else if(!strcmp(command,"hide_user_mode_changes")){
				strncpy(server->hidden_user_mode_changes,parameters,BUFFER_SIZE);
				if(strnlen(server->hidden_user_mode_changes,BUFFER_SIZE)>0){
					char tmp_buffer[BUFFER_SIZE];
					snprintf(tmp_buffer,BUFFER_SIZE,"accirc: now ignoring user mode changes %s",server->hidden_user_mode_changes);
					scrollback_output(current_server,0,tmp_buffer,TRUE);
				}else{
					scrollback_output(current_server,0,"accirc: no longer ignoring any user mode changes",TRUE);
				}
			//automatically send subsequent commands in the rc file only after a certain message is received from the server
			}else if(!strcmp(command,"post")){
				strncpy(server->post_type,parameters,BUFFER_SIZE);
				post_listen=TRUE;
				scrollback_output(current_server,0,"accirc: listening for post- commands",TRUE);
			//just a way to kick out of post mode in case you get stuck there
			}else if(!strcmp(command,"no_post")){
				post_listen=FALSE;
				scrollback_output(current_server,0,"accirc: no longer listening for post- commands",TRUE);
			//a nick to fall back to if the nick you want is taken
			}else if(!strcmp(command,"fallback_nick")){
				strncpy(server->fallback_nick,parameters,BUFFER_SIZE);
				char output_buffer[BUFFER_SIZE];
				snprintf(output_buffer,BUFFER_SIZE,"accirc: fallback_nick set to %s",server->fallback_nick);
				scrollback_output(current_server,0,output_buffer,TRUE);
			}else if(!strcmp(command,"rejoin_on_kick")){
				server->rejoin_on_kick=TRUE;
				scrollback_output(current_server,0,"accirc: rejoin_on_kick set to TRUE",TRUE);
			}else if(!strcmp(command,"no_rejoin_on_kick")){
				server->rejoin_on_kick=FALSE;
				scrollback_output(current_server,0,"accirc: rejoin_on_kick set to FALSE",TRUE);
			}else if(!strcmp(command,"reconnect")){
				server->reconnect=TRUE;
				scrollback_output(current_server,0,"accirc: reconnect set to TRUE",TRUE);
			}else if(!strcmp(command,"no_reconnect")){
				server->reconnect=FALSE;
				scrollback_output(current_server,0,"accirc: reconnect set to FALSE",TRUE);
			}else if(!strcmp(command,"manual_reconnect")){
				char old_reconnect_setting=server->reconnect;
				server->reconnect=TRUE;
				scrollback_output(current_server,0,"accirc: attempting a manual reconnect, please hold while we throw some bits through the tubes...",TRUE);
				int server_index=properly_close(current_server);
				if(server_index>=0){
					server=get_server(server_index);
					server->reconnect=old_reconnect_setting;
				}
			}else if(!strcmp(command,"log")){
				log_command();
			}else if(!strcmp(command,"no_log")){
				no_log_command();
			}else if(!strcmp(command,"rsearch")){
				rsearch_command(input_buffer,command,parameters,old_scrollback_end);
			}else if(!strcmp(command,"up")){
				up_command(input_buffer,command,parameters,old_scrollback_end);
			}else if(!strcmp(command,"down")){
				down_command(input_buffer,command,parameters,old_scrollback_end);
			}else if(!strcmp(command,"head")){
				head_command();
			}else if(!strcmp(command,"tail")){
				tail_command();
			//swap channel with the channel one index left
			}else if((!strcmp(command,"scl"))||(!strcmp(command,"swcl"))){
				swap_channel(current_server,-1);
			//swap channel with the channel one index right
			}else if((!strcmp(command,"scr"))||(!strcmp(command,"swcr"))){
				swap_channel(current_server,1);
			//swap server with the server one index left
			}else if(!strcmp(command,"swsl")){
				swap_server(current_server,-1);
			//swap server with the server one index right
			}else if(!strcmp(command,"swsr")){
				swap_server(current_server,1);
			//the "hi" and "bye" commands handle PMs as a channel
			}else if(!strcmp(command,"hi")){
				hi_command(input_buffer,command,parameters);
			}else if(!strcmp(command,"bye")){
				bye_command(input_buffer,command,parameters);
			}else if(!strcmp(command,"mode_str")){
				server->use_mode_str=TRUE;
				scrollback_output(current_server,0,"accirc: use_mode_str set to TRUE",TRUE);
			}else if(!strcmp(command,"no_mode_str")){
				server->use_mode_str=FALSE;
				scrollback_output(current_server,0,"accirc: use_mode_str set to FALSE",TRUE);
			}else if(!strcmp(command,"set_quit_msg")){
				if(!strcmp(parameters,"")){
					//re-set to default when no parameters are given
					strncpy(server->quit_msg,DEFAULT_QUIT_MESSAGE,BUFFER_SIZE);
				}else{
					strncpy(server->quit_msg,parameters,BUFFER_SIZE);
				}
				
				char notify_buffer[BUFFER_SIZE];
				snprintf(notify_buffer,BUFFER_SIZE,"accirc: quit message for this erver set to \"%s\"",server->quit_msg);
				scrollback_output(current_server,0,notify_buffer,TRUE);
			}else if(!strcmp(command,"ping_on_pms")){
				server->ping_on_pms=TRUE;
				scrollback_output(current_server,0,"accirc: will now consider every PM received on this server to be a PING",TRUE);
			}else if(!strcmp(command,"no_ping_on_pms")){
				server->ping_on_pms=FALSE;
				scrollback_output(current_server,0,"accirc: will now NOT consider every PM received on this server to be a PING (the first one still is considered a PING though)",TRUE);
			}else if(!strcmp(command,"hide_joins_quits")){
				server->hide_joins_quits=TRUE;
				scrollback_output(current_server,0,"accirc: will now HIDE all JOINs, QUITs, and PARTs for all channels of this server\n",TRUE);
			}else if(!strcmp(command,"no_hide_joins_quits")){
				server->hide_joins_quits=FALSE;
				scrollback_output(current_server,0,"accirc: will now SHOW all JOINs, QUITs, and PARTs for all channels of this server\n",TRUE);
			}else if(!strcmp(command,"disconnect")){
				//set the reconnect to false so we don't try to immediately reconnect after disconnecting
				server->reconnect=FALSE;
				
				//close the connection to this server without sending a proper QUIT message
				//this will result in a connection timeout from the perspective of the server
				//NOTE: the value of current_server will be re-set once this completes
				properly_close(current_server);
			//unknown command error
			//NOTE: prior to a command being "unknown" we check if there is an alias and try to handle it as such
			}else if(!handle_aliased_command(command,parameters)){
				char error_buffer[BUFFER_SIZE];
				snprintf(error_buffer,BUFFER_SIZE,"accirc: Err: unknown command \"%s\"",command);
				scrollback_output(current_server,0,error_buffer,TRUE);
			}
		}else{
			//the return is not used here because it's pretty inconsequential
			//if it isn't an aliased command we do nothing
			handle_aliased_command(command,parameters);
		}
	//if it's a server command send the raw text to the server
	}else if(server_command){
		//if we're connected to a server
		if(current_server>=0){
			char to_send[BUFFER_SIZE];
			snprintf(to_send,BUFFER_SIZE,"%s\n",input_buffer);
			server_write(current_server,to_send);
			
			//format the text for my viewing benefit (this is also what will go in logs, with a newline)
			char output_buffer[BUFFER_SIZE];
			snprintf(output_buffer,BUFFER_SIZE,"%s",input_buffer);
			
			//place my own text in the scrollback for this server and channel
			scrollback_output(current_server,0,output_buffer,TRUE);
			
			//refresh the channel text just in case
			refresh_channel_text();
		}
	//if it's not a command of any kind send it as a PM to current channel and server
	}else{
		privmsg_command(input_buffer);
	}
	
	strncpy(input_buffer,"\0",BUFFER_SIZE);
}


//BEGIN parse_server HELPER FUNCTIONS

//handle the "001" server command (a welcome message)
void server_001_command(irc_connection *server, char *tmp_buffer, int first_space){
	//rather than make a new buffer just use the one it'll ulitmately be stored in
	char *user_nick=server->nick;
	
	//go a space at a time until we get to the nick portion, then leave because it's set
	first_space=strfind(" ",server->read_buffer);
	substr(user_nick,server->read_buffer,first_space+1,strlen(server->read_buffer)-first_space);
	first_space=strfind(" ",user_nick);
	substr(tmp_buffer,user_nick,first_space+1,strlen(user_nick)-first_space);
	substr(user_nick,tmp_buffer,0,strfind(" ",tmp_buffer));
	
	//let the user know his nick is recognized
	refresh_server_list();
}

//handle the "332" server command (channel topic)
void server_332_command(irc_connection *server, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel){
	char channel[BUFFER_SIZE];
	char topic[BUFFER_SIZE];
	
	//pull out the topic right away, because it's delimted by " :" and so is easy
	int space_colon_index=strfind(" :",server->read_buffer);
	substr(topic,server->read_buffer,space_colon_index+2,strlen(server->read_buffer)-space_colon_index-2);
	
	strncpy(tmp_buffer,server->read_buffer,BUFFER_SIZE);
	
	//go a space at a time until we get to the relevant field
	int n;
	for(n=0;n<3;n++){
		//note I can set tmp_buffer to a substring of itself here because I'm never overwriting data I'll later need
		//it's just a left shift
		first_space=strfind(" ",tmp_buffer);
		substr(tmp_buffer,tmp_buffer,first_space+1,strlen(tmp_buffer)-first_space-1);
	}
	
	//now we're at the channel, get its name
	//we don't need to lower-case here because ch_idx_from_name does that already
	substr(channel,tmp_buffer,0,strfind(" ",tmp_buffer));
	
	//go through the channels, find out the one to output to, set "output_channel" to that index
	//note that if we never find the channel output_channel stays at its default, which is the SERVER channel
	int channel_index=ch_idx_from_name(server,channel);
	if(channel_index>=0){
		*output_channel=channel_index;
		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,channel_index)->data);
		
		snprintf(output_buffer,BUFFER_SIZE,"TOPIC for %s :%s",ch->name,topic);
		
		//store the topic in the general data structure
		strncpy(ch->topic,topic,BUFFER_SIZE);
		
		//and output
		refresh_channel_topic();
	}
}

//handle the "333" server command (channel topic timestamp)
void server_333_command(irc_connection *server, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel){
	char channel[BUFFER_SIZE];
	
	//go a space at a time until we get to the relevant field
	int n;
	for(n=0;n<2;n++){
		//note I can set tmp_buffer to a substring of itself here because I'm never overwriting data I'll later need
		//it's just a left shift
		first_space=strfind(" ",tmp_buffer);
		substr(tmp_buffer,tmp_buffer,first_space+1,strlen(tmp_buffer)-first_space-1);
	}
	
	//now we're at the channel, get its name
	first_space=strfind(" ",tmp_buffer);
	substr(channel,tmp_buffer,0,first_space);
	
	substr(tmp_buffer,tmp_buffer,first_space+1,strlen(tmp_buffer)-first_space-1);
	
	//now we're at the user who set this topic
	first_space=strfind(" ",tmp_buffer);
	char setting_user[BUFFER_SIZE];
	substr(setting_user,tmp_buffer,0,first_space);
	
	substr(tmp_buffer,tmp_buffer,first_space+1,strlen(tmp_buffer)-first_space-1);
	
	//now we're at the timestamp
	time_t timestamp=atoi(tmp_buffer);
	
	//go through the channels, find out the one to output to, set "output_channel" to that index
	//note that if we never find the channel, we output to index 0 (the SERVER channel)
	int channel_index=ch_idx_from_name(server,channel);
	if(channel_index>=0){
		*output_channel=channel_index;
	}else{
		*output_channel=0;
	}
		
	snprintf(output_buffer,BUFFER_SIZE,"Topic for %s set by %s at %s",channel,setting_user,ctime(&timestamp));
	
	//and output
	refresh_channel_topic();
}

//handle the "353" server command (names list)
void server_353_command(irc_connection *server, int server_index, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel){
	strncpy(tmp_buffer,server->read_buffer,BUFFER_SIZE);
	char channel[BUFFER_SIZE];
	int space_colon_index=strfind(" :",tmp_buffer);
	int channel_start_index=space_colon_index-1;
	while(tmp_buffer[channel_start_index]!=' '){
		channel_start_index--;
	}
	channel_start_index++;
	
	substr(channel,tmp_buffer,channel_start_index,space_colon_index-channel_start_index);
	
	//set the correct output channel
	*output_channel=find_output_channel(server,channel);
	
	//if we found this channel in our list
	if(output_channel>0){
		char names[BUFFER_SIZE];
		substr(names,tmp_buffer,space_colon_index+2,strlen(tmp_buffer)-space_colon_index-2);
		while(strlen(names)>0){
			char this_name[BUFFER_SIZE];
			int space_index=strfind(" ",names);
			
			//if there wasn't a space left, we're on the last name, just pretend there was a space after it, k?
			if(space_index==-1){
				space_index=strlen(names);
			}
			
			substr(this_name,names,0,space_index);
			substr(names,names,space_index+1,strlen(names)-space_index-1);
			
			//trim this user's name to remove the mode modifier, if applicable
			char mode_prefix[BUFFER_SIZE];
			strncpy(mode_prefix,"",BUFFER_SIZE);
			if((strfind("@",this_name)==0)||(strfind("~",this_name)==0)||(strfind("%",this_name)==0)||(strfind("&",this_name)==0)||(strfind("+",this_name)==0)){
				snprintf(mode_prefix,BUFFER_SIZE,"%c",this_name[0]);
				substr(this_name,this_name,1,strlen(this_name)-1);
			}
			
			add_name(server_index,*output_channel,this_name,mode_prefix);
		}
	}
}

//handle the "366" server command (end of names list)
void server_366_command(irc_connection *server, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel){
	//just output to the right channel
	strncpy(tmp_buffer,server->read_buffer,BUFFER_SIZE);
	char channel[BUFFER_SIZE];
	int space_colon_index=strfind(" :",tmp_buffer);
	int channel_start_index=space_colon_index-1;
	while(tmp_buffer[channel_start_index]!=' '){
		channel_start_index--;
	}
	channel_start_index++;
	
	substr(channel,tmp_buffer,channel_start_index,space_colon_index-channel_start_index);
	
	//set the correct output channel
	*output_channel=find_output_channel(server,channel);
}


//handle the "privmsg" server command
void server_privmsg_command(irc_connection *server, int server_index, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel, char *nick, char *text){
	char channel[BUFFER_SIZE];
	int space_colon_index=strfind(" :",tmp_buffer);
	substr(channel,tmp_buffer,0,space_colon_index);
	
	substr(tmp_buffer,tmp_buffer,space_colon_index+2,strlen(tmp_buffer)-space_colon_index-2);
	
	strncpy(text,tmp_buffer,BUFFER_SIZE);
	
	//set the correct output channel
	*output_channel=find_output_channel(server,channel);
	
	char tmp_nick[BUFFER_SIZE];
	strncpy(tmp_nick,server->nick,BUFFER_SIZE);
	strtolower(tmp_nick,BUFFER_SIZE);
	
	if(!strncmp(tmp_nick,channel,BUFFER_SIZE)){
		//if there is a faux PM channel for this user, send the output there, rather than treating it specially
		char lower_nick[BUFFER_SIZE];
		strncpy(lower_nick,nick,BUFFER_SIZE);
		strtolower(lower_nick,BUFFER_SIZE);
		
		if(find_output_channel(server,lower_nick)>0){
			*output_channel=find_output_channel(server,lower_nick);
			channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,*output_channel)->data);
			
			//if configured, treat all pms as pings on this server
			if(ch->is_pm){
				if(server->ping_on_pms){
					ping_log(server_index,"PM",nick,channel,text);
					ch->was_pingged=TRUE;
				}
			}
		}else{
			//there is an auto-hi option to do a /hi for a user who PMs us
			//but didn't already have a faux-channel associated with their name
			//so handle that here
			if(auto_hi){
				int current_channel=server->current_channel;
				
				//temporarily switch active server memory
				//so the faux channel gets made on the server that got the PM
				//rather than whatever server was selected at the time
				int old_server=current_server;
				current_server=server_index;
				
				char cmd_buf[BUFFER_SIZE];
				snprintf(cmd_buf,BUFFER_SIZE,"%chi %s",client_escape,nick);
				parse_input(cmd_buf,FALSE);
				
				//now that the channel is made keep the user on the server they were on
				//because they didn't really want to change that
				current_server=old_server;
				
				//at this point an output channel should exist
				//so create it, set it pingged, and add to the ping log (just for the first message)
				*output_channel=find_output_channel(server,lower_nick);
				ping_log(server_index,"PM",nick,channel,text);
				channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,*output_channel)->data);
				ch->was_pingged=TRUE;
				
				//do NOT change to this channel yet though
				//leave it as a deselected pingged channel for now
				server->current_channel=current_channel;
				
				//update the buffer to display the change
				refresh_channel_text();
			}else{
				//if we're configured to log, log PMs too, and in a more obvious way (separate file)
				ping_log(server_index,"PM",nick,channel,text);
				
				//set this to the last PM-ing user, so we can later reply if we so choose
				strncpy(server->last_pm_user,nick,BUFFER_SIZE);
				
				//set the was_pingged flag for the server in the case of a PM
				channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,0)->data);
				ch->was_pingged=TRUE;
			}
		}
	}
	
	char nick_mode_str[BUFFER_SIZE];
	strncpy(nick_mode_str,"",BUFFER_SIZE);
	
	//display channel modes with the nick if possible
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,*output_channel)->data);
	int nick_ch_idx=nick_idx(ch,nick,0);
	if((nick_ch_idx>=0) && (server->use_mode_str)){
		dlist_entry *nick_entry=dlist_get_entry(ch->user_names,nick_ch_idx);
		nick_info *nick_content=(nick_info *)(nick_entry->data);
		snprintf(nick_mode_str,BUFFER_SIZE,"%c",get_mode_prefix(nick_content->mode_str));
	}
	
	//this is so pings can be case-insensitive
	char lower_case_text[BUFFER_SIZE];
	strncpy(lower_case_text,text,BUFFER_SIZE);
	strtolower(lower_case_text,BUFFER_SIZE);
	
	//for pings
	int ping_index=ping_phrase_check(tmp_nick,ping_phrases,lower_case_text);
//	int ping_index=strfind(tmp_nick,lower_case_text);
	
	//for any CTCP message (which takes highest precedence)
	char ctcp[BUFFER_SIZE];
	snprintf(ctcp,BUFFER_SIZE,"%c",0x01);
	int ctcp_check=strfind(ctcp,text);
	
	//if there was a CTCP message
	if(ctcp_check==0){
		//if there's a space take the command to be until there
		if(strfind(" ",text)>=0){
			//the 1 here (and -1 in length) is to cut off the leading 0x01 to get the CTCP command
			substr(ctcp,text,1,strfind(" ",text)-1);
		//otherwise take it to be to the end, then check for a trailing 0x01 and cut it off if it's there
		//(this ctcp command had no arguments)
		}else{
			//the 1 here (and -1 in length) is to cut off the leading 0x01 to get the CTCP command
			substr(ctcp,text,1,strlen(text)-1);
			
			char tmp_buffer[BUFFER_SIZE];
			strncpy(tmp_buffer,ctcp,BUFFER_SIZE);
			
			//this accounts for a possible trailing byte
			snprintf(ctcp,BUFFER_SIZE,"%c",0x01);
			if(strfind(ctcp,tmp_buffer)>=0){
				tmp_buffer[strfind(ctcp,tmp_buffer)]='\0';
			}
			strncpy(ctcp,tmp_buffer,BUFFER_SIZE);
		}
		
		//be case-insensitive
		strtolower(ctcp,BUFFER_SIZE);
		
		//NOTE: timestamps are added to all output by the scrollback_output function
		
		//handle CTCP ACTION
		if(!strcmp(ctcp,"action")){
			int offset=strlen("action");
			char tmp_buffer[BUFFER_SIZE];
			//the +1 and -1 is because we want to start AFTER the ctcp command, and ctcp_check is AT that byte
			//and another +1 and -1 because we don't want to include the space the delimits the CTCP command from the rest of the message
			substr(tmp_buffer,text,ctcp_check+offset+2,strlen(text)-ctcp_check-offset-2);
			
			//this accounts for a possible trailing byte
			snprintf(ctcp,BUFFER_SIZE,"%c",0x01);
			if(strfind(ctcp,tmp_buffer)>=0){
				tmp_buffer[strfind(ctcp,tmp_buffer)]='\0';
			}
			
			//if this was also a ping, handle that too
			if(ping_index>=0){
				//if we're configured to log, log this as a ping in the pings file
				ping_log(server_index,"PING",nick,channel,text);
				
				//audio output
				beep();
				//format the output to show that we were pingged
				snprintf(output_buffer,BUFFER_SIZE,"*** *%s %s",nick,tmp_buffer);
				
				//set the was_pingged flag so the user can see that information at a glance
				ch->was_pingged=TRUE;
			//if this wasn't a ping but was a normal CTCP ACTION output for that
			}else{
				snprintf(output_buffer,BUFFER_SIZE,"*%s %s",nick,tmp_buffer);
			}
		//NOTE: VERSION string is a configuration option, so users can set it to something interesting if they want
		//handle CTCP VERSION
		}else if(!strcmp(ctcp,"version")){
			int offset=strlen("version");
			char tmp_buffer[BUFFER_SIZE];
			//the +1 and -1 is because we want to start AFTER the ctcp command, and ctcp_check is AT that byte
			//and another +1 and -1 because we don't want to include the space the delimits the CTCP command from the rest of the message
			substr(tmp_buffer,text,ctcp_check+offset+2,strlen(text)-ctcp_check-offset-2);
			
			//some clients prefer privmsg responses, others prefer notice response; we do notices
			int old_server=current_server;
			current_server=server_index;
//			snprintf(ctcp,BUFFER_SIZE,"%cprivmsg %s :%cVERSION %s%c",server_escape,nick,0x01,custom_version,0x01);
			snprintf(ctcp,BUFFER_SIZE,"%cnotice %s :%cVERSION %s%c",server_escape,nick,0x01,custom_version,0x01);
			parse_input(ctcp,FALSE);
			current_server=old_server;
			
			refresh_server_list();
			refresh_channel_list();
			refresh_channel_topic();
			refresh_channel_text();
		//handle CTCP PING
		}else if(!strcmp(ctcp,"ping")){
			int offset=strlen("ping");
			char tmp_buffer[BUFFER_SIZE];
			//the +1 and -1 is because we want to start AFTER the ctcp command, and ctcp_check is AT that byte
			//and another +1 and -1 because we don't want to include the space the delimits the CTCP command from the rest of the message
			substr(tmp_buffer,text,ctcp_check+offset+2,strlen(text)-ctcp_check-offset-2);
			
			//this response is a notice in the spec
			int old_server=current_server;
			current_server=server_index;
			snprintf(ctcp,BUFFER_SIZE,"%cnotice %s :%s",server_escape,nick,text);
			parse_input(ctcp,FALSE);
			current_server=old_server;
			
			refresh_server_list();
			refresh_channel_list();
			refresh_channel_topic();
			refresh_channel_text();
		}
	//handle for a ping (when someone says our own nick)
	//there is a was_pingged flag per channel, so we can display it as newline AND bold in the channel list output
	}else if(ping_index>=0){
		//if we're configured to log, log PINGs too, in a separate file
		ping_log(server_index,"PING",nick,channel,text);
		
		//audio output
		beep();
		//format the output to show that we were pingged
		snprintf(output_buffer,BUFFER_SIZE,"***<%s%s> %s",nick_mode_str,nick,text);
		
		//set the was_pingged flag so the user can see that information at a glance
		ch->was_pingged=TRUE;
	}else{
		//format the output of a PM in a very pretty way
		snprintf(output_buffer,BUFFER_SIZE,"<%s%s> %s",nick_mode_str,nick,text);
	}
}

//handle the "join" command from the server
void server_join_command(irc_connection *server, int server_index, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel, char *nick, char *text){
	//first get the channel name
	char channel[BUFFER_SIZE];
	
	//cut the leading : from the channel name, if there is one
	if(text[0]==':'){
		substr(channel,text,1,strlen(text)-1);
	}else{
		substr(channel,text,0,strlen(text));
	}
	
	//if it was us doing the join-ing
	if(!strncmp(server->nick,nick,BUFFER_SIZE)){
		join_new_channel(server_index,channel,output_buffer,output_channel,FALSE);
	//else it wasn't us doing the join so just output the join message to that channel (which presumably we're in)
	}else{
		int channel_index=ch_idx_from_name(server,channel);
		//someone else joined a channel that we were in, and we just found that channel (case-insensitive)
		if(channel_index>=0){
			//so add this user to that channel's names array
			nick_info *nick_content=(nick_info *)(malloc(sizeof(nick_info)));
			strncpy(nick_content->user_name,nick,BUFFER_SIZE);
			strncpy(nick_content->mode_str,"",BUFFER_SIZE);
			
			channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,channel_index)->data);
			ch->user_names=dlist_append(ch->user_names,nick_content);
			ch->nick_count++;
			
			*output_channel=channel_index;
		}
	}
}

//handle the "part" command from the server
void server_part_command(irc_connection *server, int server_index, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel, char *nick, char *text){
	char channel[BUFFER_SIZE];
	
	int space_colon_index=strfind(" :",text);
	if(space_colon_index<0){
		strncpy(channel,text,BUFFER_SIZE);
	}else{
		substr(channel,text,0,space_colon_index);
	}
	
	//if it was us doing the part
	if(!strncmp(server->nick,nick,BUFFER_SIZE)){
		leave_channel(server_index,channel);
		output_channel=0;
	//else it wasn't us doing the part so just output the part message to that channel (which presumably we're in)
	}else{
		*output_channel=find_output_channel(server,channel);
		
		del_name(server_index,*output_channel,nick);
		//note special_output is still false here, we never output up there
	}
}

//handle the "kick" command from the server
void server_kick_command(irc_connection *server, int server_index, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel, char *text){
	char channel[BUFFER_SIZE];
	substr(channel,text,0,strfind(" ",text));
	
	char kicked_user[BUFFER_SIZE];
	int space_index=strfind(" ",text);
	substr(kicked_user,text,space_index+1,strlen(text)-space_index-1);
	
	//if there was a kick message tear that out of the name (it'll still be in output buffer)
	int space_colon_index=strfind(" :",kicked_user);
	if(space_colon_index>=0){
		substr(kicked_user,kicked_user,0,space_colon_index);
	}
	
	//if we were the one who got kicked
	if(!strncmp(kicked_user,server->nick,BUFFER_SIZE)){
		leave_channel(server_index,channel);
		output_channel=0;
		
		//if we're to rejoin on a kick do that now
		if(server->rejoin_on_kick){
			int old_server=current_server;
			current_server=server_index;
			char to_parse[BUFFER_SIZE];
			snprintf(to_parse,BUFFER_SIZE,"%cjoin %s",server_escape,channel);
			parse_input(to_parse,FALSE);
			current_server=old_server;
			
			refresh_server_list();
			refresh_channel_list();
			refresh_channel_text();
		}
	//else it wasn't us getting kicked so just output the join message to that channel (which presumably we're in)
	}else{
		int channel_index=ch_idx_from_name(server,channel);
		if(channel_index>=0){
			*output_channel=channel_index;
			del_name(server_index,channel_index,kicked_user);
		}
	}
}

//handle the "nick" command from the server
void server_nick_command(irc_connection *server, int server_index, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel, char *nick, char *text, char *special_output){
	//if we changed our nick
	if(!strncmp(nick,server->nick,BUFFER_SIZE)){
		//change it in relevant data structures
		//leaving out the leading ":", if there is one
		if(text[0]==':'){
			substr(server->nick,text,1,strlen(text)-1);
		}else{
			substr(server->nick,text,0,strlen(text));
		}
		
		//and update the display to reflect this change
		refresh_server_list();
	}
	
	//set the user's nick to be lower-case for case-insensitive string matching
	strtolower(nick,BUFFER_SIZE);
	
	//unless otherwise noted don't update the last pm-d user
	char update_pm_user=FALSE;
	
	char lower_last_pm_user[BUFFER_SIZE];
	strncpy(lower_last_pm_user,server->last_pm_user,BUFFER_SIZE);
	strtolower(lower_last_pm_user,BUFFER_SIZE);
	
	//if this is the last user we PM-d, update that
	if(!strncmp(nick,lower_last_pm_user,BUFFER_SIZE)){
		//we can't update directly here because we haven't parsed out the new nick yet, so set a flag and we'll do so when we get there
		update_pm_user=TRUE;
	}
	
	dlist_entry *ch_entry=server->ch;
	int channel_index=0;
	while(ch_entry!=NULL){
		channel_info *ch=(channel_info *)(ch_entry->data);
		
		int name_index=nick_idx(ch,nick,0);
		//found it!
		if(name_index>=0){
			dlist_entry *nick_entry=dlist_get_entry(ch->user_names,name_index);
			nick_info *nick_content=(nick_info *)(nick_entry->data);
			
			//output to the appropriate channel
			scrollback_output(server_index,channel_index,output_buffer,TRUE);
			
			char new_nick[BUFFER_SIZE];
			if(text[0]==':'){
				substr(new_nick,text,1,strlen(text)-1);
			}else{
				substr(new_nick,text,0,strlen(text));
			}
			
			//if we were pm-ing with this user, update that reference
			if(update_pm_user){
				strncpy(server->last_pm_user,new_nick,BUFFER_SIZE);
			}
			
			//TODO: if there was a is_pm channel named after this user it should be changed
			//(because there are files open for logs and things this isn't done now, it's a major pain)
			
			//don't actually change the name in the list if it's a PM
			//since PMs aren't channels and the nick is the defining characteristic of a PM channel
			//and so shouldn't be changed
			if(ch->is_pm==FALSE){
				//update this user's entry in that channel's names array
				strncpy(nick_content->user_name,new_nick,BUFFER_SIZE);
				
				//note modes do not change in the case of a nick change
			}
			
			//we found a channel with this nick, so we've already done special output
			//no need to output again to server area
			*special_output=TRUE;
		}
		
		ch_entry=ch_entry->next;
		channel_index++;
	}
}

//handle the "topic" command from the server
void server_topic_command(irc_connection *server, int server_index, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel, char *nick, char *text){
	char channel[BUFFER_SIZE];
	int space_colon_index=strfind(" :",tmp_buffer);
	substr(channel,tmp_buffer,0,space_colon_index);
	
	substr(tmp_buffer,tmp_buffer,space_colon_index+2,strlen(tmp_buffer)-space_colon_index-2);
	
	strncpy(text,tmp_buffer,BUFFER_SIZE);
	
	//lower case the channel so we can do a case-insensitive string match against it
	strtolower(channel,BUFFER_SIZE);
	
	//output to the correct place
	*output_channel=find_output_channel(server,channel);
	
	//update the topic for this channel on this server
	//leaving out the leading ":", if there is one
	unsigned int offset=(text[0]==':')?1:0;
	channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,*output_channel)->data);
	substr(ch->topic,text,offset,strlen(text)-offset);
	
	//update the display
	refresh_channel_topic();
}

//handle the "mode" command from the server
//returns TRUE for special_output, otherwise FALSE
//accepts formats:
//":Shishichi!notIRCuser@hide-4C94998D.fidnet.com MODE #FaiD3.0 +o Nick"
//":*.DE MODE #imgurians +o Nick"
//":bitlbee.local MODE &bitlbee +v Nick"
char server_mode_command(irc_connection *server, int server_index, char *text, int *output_channel){
	char special_output=FALSE;
	char channel[BUFFER_SIZE];
	substr(channel,text,0,strfind(" ",text));
	
	//output to the correct place
	*output_channel=find_output_channel(server,channel);
	
	//update user modes as needed
	
	char tmp_text[BUFFER_SIZE];
	int space_idx=strfind(" ",text);
	substr(tmp_text,text,space_idx+1,strlen(text)-1-space_idx);
	
	//get the +o -o +v -v +ooo +h etc. string
	char mode_ctrl_str[BUFFER_SIZE];
	space_idx=strfind(" ",tmp_text);
	substr(mode_ctrl_str,tmp_text,0,space_idx);
	
	//if this mode change should be hidden per the user's setting for this server
	unsigned int n=0;
	for(n=0;n<strnlen(server->hidden_user_mode_changes,BUFFER_SIZE);n++){
		char tmp_buffer[2];
		tmp_buffer[0]=server->hidden_user_mode_changes[n];
		tmp_buffer[1]='\0';
		if(strfind(tmp_buffer,mode_ctrl_str)>=0){
			//then don't output it, just hide it
			//NOTE: this doesn't change our internal processing at all
			//it's only for output
			special_output=TRUE;
		}
	}
	
	//get the string of nicks to apply the mode_ctrl_str to
	char nicks[BUFFER_SIZE];
	substr(nicks,tmp_text,space_idx+1,strlen(tmp_text)-1-space_idx);

	/*
	fprintf(error_file,"dbg: Got MODE command \"%s\"; mode_ctrl_str=\"%s\"; tmp_text=\"%s\", nicks=\"%s\"\n",
		text,mode_ctrl_str,tmp_text,nicks);
	*/
	
	unsigned int nick_mode_idx=0;
	
	//for each nick, set mode_str in *output_channel to new_mode_str
	while(strncmp(nicks,"",BUFFER_SIZE)!=0){
		char nick[BUFFER_SIZE];
		space_idx=strfind(" ",nicks);
		if(space_idx<0){
			strncpy(nick,nicks,BUFFER_SIZE);
			strncpy(nicks,"",BUFFER_SIZE);
		}else{
			substr(nick,nicks,0,space_idx);
			char tmp_buffer[BUFFER_SIZE];
			substr(tmp_buffer,nicks,space_idx+1,strlen(nicks)-1);
			strncpy(nicks,tmp_buffer,BUFFER_SIZE);
		}
		
		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,*output_channel)->data);
		int name_index=nick_idx(ch,nick,0);
		if(name_index>=0){
			char new_mode_str[BUFFER_SIZE];
			
			//use the existing mode string for this user in this channel
			//as a starting point
			strncpy(new_mode_str,((nick_info *)(dlist_get_entry(ch->user_names,name_index)->data))->mode_str,BUFFER_SIZE);
			
			//NOTE: this handles multiuser syntax +ooo nick nick nick
			//NOTE: this handles mode losses (-o, -ooo) in addition to mode grants
			if((nick_mode_idx+1)<strlen(mode_ctrl_str)){
				//get the mode character that's being applied or removed
				char mode_delta=mode_ctrl_str[nick_mode_idx+1];
				
				//copy the one-character mode into a string
				//so that it can be used with strfind
				char mode_delta_str[2];
				mode_delta_str[0]=mode_ctrl_str[nick_mode_idx+1];
				mode_delta_str[1]='\0';
				
				//find the existing index of this mode in the mode list, if it exists
				int mode_idx=strfind(mode_delta_str,new_mode_str);
				
				//if a new mode is being added
				if(mode_ctrl_str[0]=='+'){
					//and that mode wasn't previously included in the list
					if(mode_idx<0){
						//then append it to the end
						unsigned int ins_idx=strlen(new_mode_str);
						if((ins_idx+1)<BUFFER_SIZE){
							new_mode_str[ins_idx]=mode_delta;
							new_mode_str[ins_idx+1]='\0';
						}
					}
				//if an existing mode is being removed
				}else if(mode_ctrl_str[0]=='-'){
					//and that mode was included in the mode string for this user previously
					if(mode_idx>=0){
						//then remove it by copying the existing string,
						//omitting the character at that index where this mode was found
						
						char tmp_buffer[BUFFER_SIZE];
						unsigned int idx;
						for(idx=0;idx<mode_idx;idx++){
							tmp_buffer[idx]=new_mode_str[idx];
						}
						for(idx=mode_idx+1;idx<strlen(new_mode_str);idx++){
							//NOTE: because one character from the mode string is removed/omitted
							//we need a -1 here to sync up destination and source indicies
							tmp_buffer[idx-1]=new_mode_str[idx];
						}
						//always null-terminate; realizing that one character was removed
						tmp_buffer[strlen(new_mode_str)-1]='\0';
						
						strncpy(new_mode_str,tmp_buffer,BUFFER_SIZE);
					}
				}
				/*
				fprintf(error_file,"dbg: New user mode for user \"%s\" in channel \"%s\" is user_mode_str \"%s\"\n",
					nick,ch->name,new_mode_str);
				*/
			}
			
			//copy the newly-transformed mode back into the user name information
			strncpy(((nick_info *)(dlist_get_entry(ch->user_names,name_index)->data))->mode_str,new_mode_str,BUFFER_SIZE);
		}
		
		nick_mode_idx++;
	}
	
	return special_output;
}

//handle the "quit" command from the server
void server_quit_command(irc_connection *server, int server_index, char *tmp_buffer, int first_space, char *output_buffer, int *output_channel, char *nick, char *text, char *special_output){
	//set the user's nick to be lower-case for case-insensitive string matching
	strtolower(nick,BUFFER_SIZE);
	
	int channel_index=0;
	dlist_entry *ch_entry=server->ch;
	while(ch_entry!=NULL){
		channel_info *ch=(channel_info *)(ch_entry->data);
		
		int name_index=nick_idx(ch,nick,0);
		//found it!
		if(name_index>=0){
			//if we're not hiding joins and quits on this server
			if(!(server->hide_joins_quits)){
				//output to the appropriate channel
				scrollback_output(server_index,channel_index,output_buffer,TRUE);
			}
			
			//don't actually remove the name from the list if it's a PM
			//since PMs aren't channels and you can't part from them
			if(ch->is_pm==FALSE){
				//remove this user from that channel's names array
				ch->user_names=dlist_delete_entry(ch->user_names,name_index,TRUE);
				//and update the user count
				ch->nick_count--;
			}
			
			//for handling later; just let us know we found a channel to output to
			*special_output=TRUE;
		}
		
		ch_entry=ch_entry->next;
		channel_index++;
	}
}

//END parse_server HELPER FUNCTIONS

//parse incoming data from a server
//returns the server_index if the connection persists (it can change on reconnect)
//returns -1 if the server was cleaned up and no longer exists
int parse_server(int server_index){
	irc_connection *server=get_server(server_index);
	
	//clear out the remainder of the buffer since we re-use this memory
	int n;
	for(n=strlen(server->read_buffer);n<BUFFER_SIZE;n++){
		server->read_buffer[n]='\0';
	}
	
	//update the timestamp of the last message from this server
	//because we just got a message from this server (of course)
	server->last_msg_time=time(NULL);
	
	//parse in whatever the server sent and display it appropriately
	int first_delimiter=strfind(" :",server->read_buffer);
	//the command
	char command[BUFFER_SIZE];
	//the parameters, note that this includes the trailing newline
	char parameters[BUFFER_SIZE];
	if(first_delimiter>0){
		substr(command,server->read_buffer,0,first_delimiter);
		substr(parameters,server->read_buffer,first_delimiter+strlen(" :"),strlen(server->read_buffer)-first_delimiter);
	}else{
		strncpy(command,"",BUFFER_SIZE);
		strncpy(parameters,"",BUFFER_SIZE);
	}
	
	//respond to server pings, silently
	if(!strcmp(command,"PING")){
		char to_send[BUFFER_SIZE];
		snprintf(to_send,BUFFER_SIZE,"PONG :%s",parameters);
		server_write(server_index,to_send);
	//if we got an error, close the link and clean up the structures
	}else if(!strcmp(command,"ERROR")){
		server_index=properly_close(server_index);
	}else{
		//whether or not this is a JOIN, PART, or QUIT message
		//because in some circumstances we hide those
		char is_join_part_quit=FALSE;
				
		//set this to show as having new data, it must since we're getting something on it
		//(this is done automatically as a result of new_content being set true in scrollback_output)
		refresh_server_list();
		
		//take out the trailing newline (accounting for the possibility of windows newlines)
		int newline_index=strfind("\r\n",server->read_buffer);
		if(newline_index<0){
			//the possibility, not the necessity; proper newlines are accepted also
			newline_index=strfind("\n",server->read_buffer);
		}
		//NOTE: I can set this to be a substring of itself since I'm not overwriting anything during copy that I'll need
		substr(server->read_buffer,server->read_buffer,0,newline_index);
		
		//a flag to say if we already output within the handling
		//(if so don't output again at the end)
		char special_output=FALSE;
		
		//what gets shown in logs and user scrollback, which may differ from the raw server data
		char output_buffer[BUFFER_SIZE];
		strncpy(output_buffer,server->read_buffer,BUFFER_SIZE);
		
		//the channel to output to, by default the SYSTEM channel
		int output_channel=0;
		
		//seperate server messages from PMs
		int first_space=strfind(" ",server->read_buffer);
		if(first_space>=0){
			//the start at 1 is to cut off the preceding ":"
			//remember the second arguement to substr is a LENGTH, not an index
			substr(command,server->read_buffer,1,first_space-1);
			
			//TODO: make this less hacky, it works but... well, hacky
			//NOTE: checking for the literal server name was giving me issues because sometimes a server will re-direct to another one, so this just checks in general "is it any valid server name?"
			
//			//if this message started with the server's name
//			if(!strncmp(command,server->server_name,BUFFER_SIZE)){
			
			//check that it is NOT a user (meaning it must not have the delimiter chars for a username)
			if(strfind("@",command)==-1){
				//these messages have the form ":naos.foonetic.net 001 accirc_user :Welcome to the Foonetic IRC Network nick!realname@hostname.could.be.ipv6"
				substr(command,server->read_buffer,1,strlen(server->read_buffer)-1);
				
				first_space=strfind(" ",command);
				char tmp_buffer[BUFFER_SIZE];
				substr(tmp_buffer,command,first_space+1,strlen(command)-first_space-1);
				substr(command,tmp_buffer,0,strfind(" ",tmp_buffer));
				
				//firstly, if this is something we were waiting on, then start sending the text we were waiting to send
				//(as it is now "hammertime")
				if(!strncmp(command,server->post_type,BUFFER_SIZE)){
					//remember what server the user was on, because we need to hop over to the one that just sent us a message
					int old_server=current_server;
					current_server=server_index;

					//let the user know we are going to do something
					char notify_buffer[BUFFER_SIZE];
					snprintf(notify_buffer,BUFFER_SIZE,"accirc: Sending %i post-%s commands now...",dlist_length(server->post_commands),server->post_type);
					scrollback_output(current_server,0,notify_buffer,TRUE);
					
					//send the post commands (which are stored in a doubly-linekd list)
					//NOTE: this +1 is important because post_command_entry->data
					//(from server->post_commands) is malloc'd to BUFFER_SIZE+1
					char tmp_command_buffer[BUFFER_SIZE+1];
					dlist_entry *post_command_entry=server->post_commands;
					
					//loop through the post commands
					while(post_command_entry!=NULL){
						//send one at a time
						strncpy(tmp_command_buffer,(char *)(post_command_entry->data),BUFFER_SIZE+1);
						parse_input(tmp_command_buffer,FALSE);
						
						post_command_entry=post_command_entry->next;
					}
					
					//go back to the user-specified server now that we're done parsing
					current_server=old_server;
					
					//refresh the channel text display since we just switched servers and switched back
					refresh_channel_text();
				}
				
				//welcome message (we set the server NICK data here since it's clearly working for us)
				if(!strcmp(command,"001")){
					server_001_command(server,tmp_buffer,first_space);
				//current channel topic
				}else if(!strcmp(command,"332")){
					server_332_command(server,tmp_buffer,first_space,output_buffer,&output_channel);
				//handle time set information for a channel topic
				}else if(!strcmp(command,"333")){
					server_333_command(server,tmp_buffer,first_space,output_buffer,&output_channel);
				//names list
				//(like this: ":naos.foonetic.net 353 accirc_user @ #FaiD3.0 :accirc_user @neutrak @NieXS @cheese @MonkeyofDoom @L @Data @Spock ~Shishichi davean")
				//(or this: ":naos.foonetic.net 353 neutrak = #FaiD :neutrak mo0 Decarabia Gelsamel_ NieXS JoeyJo0 cheese")
				}else if(!strcmp(command,"353")){
					server_353_command(server,server_index,tmp_buffer,first_space,output_buffer,&output_channel);
				//end of names list
				}else if(!strcmp(command,"366")){
					server_366_command(server,tmp_buffer,first_space,output_buffer,&output_channel);
				//end of message of the day (useful as a delimiter)
				}else if(!strcmp(command,"376")){
					
				//nick already in use, so try a new one
				}else if(!strcmp(command,"433")){
					char new_nick[BUFFER_SIZE];
					snprintf(new_nick,BUFFER_SIZE,"%s_",server->fallback_nick);
					//in case this fails again start with another _ for the next try
					strncpy(server->fallback_nick,new_nick,BUFFER_SIZE);
					
					snprintf(new_nick,BUFFER_SIZE,"NICK %s\n",server->fallback_nick);
					server_write(server_index,new_nick);
				}
			//a message from another user
			}else{
				//a temporary buffer to store intermediate results during parsing
				char tmp_buffer[BUFFER_SIZE];
				//initialized to be the value of server->read_buffer
				strncpy(tmp_buffer,server->read_buffer,BUFFER_SIZE);
				
				//NOTE: we parse out command_and_args and split on space BEFORE anything else
				//to handle uncommon MODE commands that don't contain all the normal parts
				//	normal: ":Shishichi!notIRCuser@hide-4C94998D.fidnet.com MODE #FaiD3.0 +o MonkeyofDoom"
				//	weird: ":*.DE MODE #imgurians +o Nick"
				//	weird: ":bitlbee.local MODE &bitlbee +v Nick"
				
				//the most important character to parse first is " "
				//everything that's prior to that is all the user and hostmask information we got
				//which is sometimes not much at all
				int space_index=strfind(" ",tmp_buffer);
				
				char command_and_args[BUFFER_SIZE];
				substr(command_and_args,tmp_buffer,space_index+1,strlen(tmp_buffer)-space_index-1);
				
				//declarations for various things worth parsing out
				char nick[BUFFER_SIZE];
				char real_name[BUFFER_SIZE];
				char hostmask[BUFFER_SIZE];
				char command[BUFFER_SIZE];
				char text[BUFFER_SIZE];
				
				//NOTE: substr never allows operations on negative-length substrings
				//and essentially ignores it and treats the result as a null string if given a negative length
				//so these substr calls are safe despite not explicitly verifying that the indicies are positive
				
				//user's nickname is delimeted by "!"
				int exclam_index=strfind("!",tmp_buffer);
				//start at 1 to cut off the leading ":"
				substr(nick,tmp_buffer,1,exclam_index-1);
				
				//move past that point so we have a tmp_buffer after it (we'll be doing this a lot)
				//I CAN set tmp_buffer to a substring of itself here BECAUSE it'll just shift everything left and never overwrite what it needs to use
				substr(tmp_buffer,tmp_buffer,exclam_index+1,strlen(server->read_buffer)-exclam_index-1);
				
				//user's real name is delimeted by "@"
				int at_index=strfind("@",tmp_buffer);
				substr(real_name,tmp_buffer,0,at_index);
				
				//I CAN set tmp_buffer to a substring of itself here BECAUSE it'll just shift everything left and never overwrite what it needs to use
				substr(tmp_buffer,tmp_buffer,at_index+1,strlen(tmp_buffer)-at_index-1);
				
				//hostmask is delimited by " "
				space_index=strfind(" ",tmp_buffer);
				substr(hostmask,tmp_buffer,0,space_index);
				
				substr(tmp_buffer,tmp_buffer,space_index+1,strlen(tmp_buffer)-space_index-1);
				
				//NOTE: we are using command_and_args here, parsed out earlier in this function
				//and NOT tmp_buffer
				//to account for unusual formats (MODEs)
				//e.g.
				//":*.DE MODE #imgurians +o Nick"
				//":bitlbee.local MODE &bitlbee +v Nick"
				
				//command is delimeted by " "
				space_index=strfind(" ",command_and_args);
				substr(command,command_and_args,0,space_index);
				
				//and then we copy FROM command_and_args into the tmp_buffer
				//since that contains everything after the first space in the original line
				substr(tmp_buffer,command_and_args,space_index+1,strlen(command_and_args)-space_index-1);
				
				//the rest of the string is the text
				strncpy(text,tmp_buffer,BUFFER_SIZE);
				
				//start of command handling
				//the most common message, the PM
				//":neutrak!neutrak@hide-F99E0499.device.mst.edu PRIVMSG accirc_user :test"
				if(!strcmp(command,"PRIVMSG")){
					server_privmsg_command(server,server_index,tmp_buffer,first_space,output_buffer,&output_channel,nick,text);
				//":accirc_2!1@hide-68F46812.device.mst.edu JOIN :#FaiD3.0"
				}else if(!strcmp(command,"JOIN")){
					server_join_command(server,server_index,tmp_buffer,first_space,output_buffer,&output_channel,nick,text);
					is_join_part_quit=TRUE;
				//or ":neutrak_accirc!1@sirc-8B6227B6.device.mst.edu PART #randomz"
				}else if(!strcmp(command,"PART")){
					server_part_command(server,server_index,tmp_buffer,first_space,output_buffer,&output_channel,nick,text);
					is_join_part_quit=TRUE;
				//or ":Shishichi!notIRCuser@hide-4C94998D.fidnet.com KICK #FaiD3.0 accirc_user :accirc_user: I need a kick message real quick"
				}else if(!strcmp(command,"KICK")){
					server_kick_command(server,server_index,tmp_buffer,first_space,output_buffer,&output_channel,text);
				//":accirc!1@hide-68F46812.device.mst.edu NICK :accirc_2"
				//handle for NICK changes, especially the special case of our own, where server[server_index]->nick should get reset
				//NICK changes are server-wide so I'll only be able to handle this better once I have a list of users in each channel
				}else if(!strcmp(command,"NICK")){
					server_nick_command(server,server_index,tmp_buffer,first_space,output_buffer,&output_channel,nick,text,&special_output);
				//handle for topic changes
				//":accirc_user!1@hide-68F46812.device.mst.edu TOPIC #FaiD3.0 :Welcome to #winfaid 4.0, now with grammar checking"
				}else if(!strcmp(command,"TOPIC")){
					server_topic_command(server,server_index,tmp_buffer,first_space,output_buffer,&output_channel,nick,text);
				//":Shishichi!notIRCuser@hide-4C94998D.fidnet.com MODE #FaiD3.0 +o MonkeyofDoom"
				//":*.DE MODE #imgurians +o Nick"
				//":bitlbee.local MODE &bitlbee +v Nick"
				}else if(!strcmp(command,"MODE")){
					special_output=server_mode_command(server,server_index,text,&output_channel);
				//proper NOTICE handling is to output to the correct channel if it's a channel-wide notice, and like a PM otherwise
				}else if(!strcmp(command,"NOTICE")){
					//parse out the channel
					char channel[BUFFER_SIZE];
					int space_colon_index=strfind(" :",tmp_buffer);
					substr(channel,tmp_buffer,0,space_colon_index);
					
					substr(tmp_buffer,tmp_buffer,space_colon_index+2,strlen(tmp_buffer)-space_colon_index-2);
					
					strncpy(text,tmp_buffer,BUFFER_SIZE);
					
					//output to the correct place
					output_channel=find_output_channel(server,channel);
				//using channel names lists, output quits to the correct channel
				//(this will require outputting multiple times, which I don't have the faculties for at the moment)
				}else if(!strcmp(command,"QUIT")){
					server_quit_command(server,server_index,tmp_buffer,first_space,output_buffer,&output_channel,nick,text,&special_output);
					is_join_part_quit=TRUE;
				}
			}
		}
		
		//if this is something we're supposed to hide based on server settings
		if((is_join_part_quit) && (server->hide_joins_quits)){
			//then that's special too
			special_output=TRUE;
		}
		
		//if we haven't already done some crazy kind of output
		if(!special_output){
			//do the normal kind of output
			scrollback_output(server_index,output_channel,output_buffer,TRUE);
		}
	}
	
	//if we didn't close the connection up there
	if(server_index>=0){
		server=get_server(server_index);
		
		//clear out the real buffer for the next line from the server
		for(n=0;n<BUFFER_SIZE;n++){
			server->read_buffer[n]='\0';
		}
	}
	
	return server_index;
}

//force resize detection
void force_resize(char *input_buffer, int cursor_pos, int input_display_start){
	//if we had windows allocated to start with
	if(server_list!=NULL){
		//de-allocate existing windows so as not to waste RAM
		delwin(server_list);
		delwin(channel_list);
		delwin(channel_topic);
		delwin(top_border);
		delwin(channel_text);
		delwin(bottom_border);
		delwin(user_input);
		
		server_list=NULL;
		channel_list=NULL;
		channel_topic=NULL;
		top_border=NULL;
		channel_text=NULL;
		bottom_border=NULL;
		user_input=NULL;
	}
	
	//if we were outputting fullscreen text
	//then clear out that buffer now since the screen sized changed
	//and we may or may not output fullscreen text again
	if(ncurses_fullscreen_text!=NULL){
		delwin(ncurses_fullscreen_text);
		ncurses_fullscreen_text=NULL;
	}
	
	//restart ncurses interface
	endwin();
	refresh();
	clear();
	//set some common options
	noecho();
	if(has_colors()){
#ifdef MIRC_COLOR
		start_color();
		//init colors (for MIRC color support, among other things)
		init_color(MIRC_WHITE,1000,1000,1000);
		init_color(MIRC_BLACK,0,0,0);
		init_color(MIRC_BLUE,0,0,1000);
		init_color(MIRC_GREEN,0,1000,0);
		init_color(MIRC_RED,1000,0,0);
		init_color(MIRC_BROWN,400,400,0);
		init_color(MIRC_PURPLE,600,300,900);
		init_color(MIRC_ORANGE,900,400,0);
		init_color(MIRC_YELLOW,1000,1000,0);
		init_color(MIRC_LIGHT_GREEN,400,900,400);
		init_color(MIRC_TEAL,0,1000,1000);
		init_color(MIRC_LIGHT_CYAN,600,1000,1000);
		init_color(MIRC_LIGHT_BLUE,400,400,1000);
		init_color(MIRC_PINK,1000,200,1000);
		init_color(MIRC_GREY,400,400,400);
		init_color(MIRC_LIGHT_GREY,700,700,700);
		
		int n0;
		for(n0=0;n0<MIRC_COLOR_MAX;n0++){
			int n1;
			for(n1=0;n1<MIRC_COLOR_MAX;n1++){
				int foreground=n0;
				int background=n1;
				init_pair((foreground<<4)|(background<<0),n0,n1);
			}
		}
		
		//start with a sane default color
		wcoloron(stdscr,MIRC_WHITE,MIRC_BLACK);
#endif
	}
	//get raw input
	raw();
	//set the correct terminal size constraints before we go crazy and allocate windows with the wrong ones
	getmaxyx(stdscr,height,width);
	
#ifdef DEBUG
	char error_buffer[BUFFER_SIZE];
	snprintf(error_buffer,BUFFER_SIZE,"Info: force_resize got resize event with height=%i and width=%i",height,width);
//	error_log(error_buffer);
	
	//TODO: figure out why when we size below MIN_HEIGHT and then resize back up, the accirc display doesn't come back
	//also, when resizing below MIN_HEIGHT, the error message isn't printed after the first resize event
	//but I've verified using the above debug output that this function IS getting called for all resize events
	//however for some reason the display isn't refreshing when it should
#endif
	
	//if the window is too small to display the interface
	//just show a blank display
	//(or a message like "accirc: Warn: terminal too small, make this window larger to see irc" if there is sufficient space to show it)
	//instead of hard crashing and disconnecting everything
	if((height<MIN_HEIGHT)||(width<MIN_WIDTH)){
		ncurses_fullscreen_text=newwin(height,width,0,0);
		wblank(ncurses_fullscreen_text,width,height);
		
		int error_line_cnt=3;
		
		char error_lines[error_line_cnt][BUFFER_SIZE];
		snprintf(error_lines[0],BUFFER_SIZE,"accirc: Warn: terminal too small");
		snprintf(error_lines[1],BUFFER_SIZE,"accirc: Warn: need width>=%i and height>=%i",MIN_WIDTH,MIN_HEIGHT);
		snprintf(error_lines[2],BUFFER_SIZE,"accirc: Expand to see irc");
		
		if(height>error_line_cnt){
			for(int err_line_idx=0;err_line_idx<error_line_cnt;err_line_idx++){
				if(width>strnlen(error_lines[err_line_idx],BUFFER_SIZE)){
					wmove(ncurses_fullscreen_text,err_line_idx,0);
					wprintw(ncurses_fullscreen_text,error_lines[err_line_idx]);
				}
			}
			wmove(ncurses_fullscreen_text,(height-1),0);
			wrefresh(ncurses_fullscreen_text);
		}
		//TODO: set timeouts for non-blocking?
		//TODO: figure out why the window size isn't re-detected and the error message isn't cleared when the window once again becomes sufficiently large
		keypad(ncurses_fullscreen_text,TRUE);
		//set timeouts for non-blocking
		timeout(1);
		wtimeout(ncurses_fullscreen_text,5);
		return;
	}
	
	//allocate windows for our toolbars and the main chat
	server_list=newwin(1,width,0,0);
	channel_list=newwin(1,width,1,0);
	channel_topic=newwin(1,width,2,0);
	top_border=newwin(1,width,3,0);
	channel_text=newwin((height-RESERVED_LINES),width,4,0);
	bottom_border=newwin(1,width,(height-2),0);
	user_input=newwin(1,width,(height-1),0);
	
#ifdef MIRC_COLOR
	//set sane default colors
	wcoloron(server_list,MIRC_WHITE,MIRC_BLACK);
	wcoloron(channel_list,MIRC_WHITE,MIRC_BLACK);
	wcoloron(channel_topic,MIRC_WHITE,MIRC_BLACK);
	wcoloron(top_border,MIRC_WHITE,MIRC_BLACK);
	wcoloron(channel_text,MIRC_WHITE,MIRC_BLACK);
	wcoloron(bottom_border,MIRC_WHITE,MIRC_BLACK);
	wcoloron(user_input,MIRC_WHITE,MIRC_BLACK);
#endif
	
	keypad(user_input,TRUE);
	//set timeouts for non-blocking
	timeout(1);
	wtimeout(user_input,5);
	
	wblank(server_list,width,1);
	wblank(channel_list,width,1);
	wblank(channel_topic,width,1);
	wblank(top_border,width,1);
	wblank(channel_text,width,height-RESERVED_LINES);
	wblank(bottom_border,width,1);
	wblank(user_input,width,1);
	
	//always bold the borders
	wattron(top_border,A_BOLD);
	wattron(bottom_border,A_BOLD);
	
	//if we have any kind of connection
	if(current_server>=0){
		//call the appropriate refreshes for various windows
		refresh_server_list();
		refresh_channel_list();
		refresh_channel_topic();
		refresh_channel_text();
	//if we have no connection reflect that
	}else{
		wprintw(server_list,"(no servers)");
		wprintw(channel_list,"(no channels)");
		wprintw(channel_topic,"(no channel topic)");
		wprintw(channel_text,"(no channel text)");
	}
	
	int n;
	for(n=0;n<width;n++){
		wprintw(top_border,"-");
	}
	
	//unix epoch clock (initialization)
	char time_buffer[BUFFER_SIZE];
	time_t old_time=time(NULL);
	custom_format_time(time_buffer,old_time);
	wprintw(bottom_border,time_buffer);
	
	for(n=strlen(time_buffer);n<width;n++){
		wprintw(bottom_border,"-");
	}
	
	wrefresh(server_list);
	wrefresh(channel_list);
	wrefresh(channel_topic);
	wrefresh(top_border);
	wrefresh(channel_text);
	wrefresh(bottom_border);
	wrefresh(user_input);
	
	refresh_user_input(input_buffer,cursor_pos,input_display_start);
}

//check every server we're connected to for new data and handle it accordingly
void read_server_data(){
	//a string buffer to store any errors
	char error_buffer[BUFFER_SIZE];
	
	//loop through servers and see if there's any data worth reading
	int server_index;
	int server_count=dlist_length(servers);
	for(server_index=0;server_index<server_count;server_index++){
		//if this is a valid server connection we have a buffer allocated
		irc_connection *server=get_server(server_index);
		
		//if this is not a valid server then stop
		//valid servers are always in sequence because that's how the lists work,
		//so one invalid server indicates we're done for this read loop
		if(server==NULL){
			break;
		}
		
		//read a buffer_size at a time for speed (if you read a character at a time the kernel re-schedules that each operation)
		char server_in_buffer[BUFFER_SIZE];
		int bytes_transferred;
		//the -1 here is so we always have a byte for null-termination
#ifdef _OPENSSL
		if(server->use_ssl){
			bytes_transferred=SSL_read(server->ssl_handle,server_in_buffer,BUFFER_SIZE-1);
		}else{
#endif
		bytes_transferred=read(server->socket_fd,server_in_buffer,BUFFER_SIZE-1);
#ifdef _OPENSSL
		}
#endif
		
		if((bytes_transferred<=0)&&(errno!=EAGAIN)){
			//handle connection errors gracefully here (as much as possible)
			snprintf(error_buffer,BUFFER_SIZE,"Err: connection error with server %i, host %s",server_index,server->server_name);
			error_log(error_buffer);
			//if this server wasn't automatically reconnected
			if(properly_close(server_index)<0){
				//then cancel this read operation (we'll read buffers from other servers on the next cycle)
				break;
			}
		//if this server hasn't sent anything in a while, then assume it's dead and close it properly
		//note if reconnect was set, a reconnection will be attempted
		}else if((bytes_transferred<=0)&&(errno==EAGAIN)&&((time(NULL)-(server->last_msg_time))>=SERVER_TIMEOUT)){
			//handle connection timeouts gracefully here (as much as possible)
			snprintf(error_buffer,BUFFER_SIZE,"Err: connection timed out with server %i, host %s",server_index,server->server_name);
			error_log(error_buffer);
			if(properly_close(server_index)<0){
				//then cancel this read operation (we'll read buffers from other servers on the next cycle)
				break;
			}
		}else if(bytes_transferred>0){
			//null-terminate the C string
			server_in_buffer[bytes_transferred]='\0';
			
			//note: if there's nothing in the parse_queue waiting then this does nothing but a copy from server_in_buffer
			char tmp_buffer[2*BUFFER_SIZE];
			snprintf(tmp_buffer,2*BUFFER_SIZE,"%s%s",server->parse_queue,server_in_buffer);
			strncpy(server->parse_queue,tmp_buffer,2*BUFFER_SIZE);
			
			int queue_index;
			char accumulator[BUFFER_SIZE];
			
			//clear the accumulator to initialize it
			int n;
			for(n=0;n<BUFFER_SIZE;n++){
				accumulator[n]='\0';
			}
			
			for(queue_index=0;server->parse_queue[queue_index]!='\0';queue_index++){
				if(strinsert(accumulator,server->parse_queue[queue_index],strlen(accumulator),BUFFER_SIZE)){
					if(server->parse_queue[queue_index]=='\n'){
						//clear the existing read buffer
						int n;
						for(n=0;n<BUFFER_SIZE;n++){
							server->read_buffer[n]='\0';
						}
						
						strncpy(server->read_buffer,accumulator,BUFFER_SIZE);
						server_index=parse_server(server_index);
						strncpy(accumulator,"",BUFFER_SIZE);
						
						//if we just lost connection and cleaned up, stop
						if(server_index<0){
							break;
						}
					}
				//oh shit, we overflowed the buffer
				}else{
					//tell the user this happened
					char error_buffer[BUFFER_SIZE];
					snprintf(error_buffer,BUFFER_SIZE,"accirc: Err: read queue has overflowed (nothing we can do), clearing");
					scrollback_output(server_index,0,error_buffer,TRUE);
					
					//we can't do anything but clear this and ignore it, since we could end up reading garbage data
					int n;
					for(n=0;n<BUFFER_SIZE;n++){
						server->read_buffer[n]='\0';
					}
					
					//treat this as a lost connection
					server_index=properly_close(server_index);
					break;
				}
			}
			
			//if we didn't lose connection and clean up after parsing what we already got
			if(server_index>=0){
				//the parse queue is everything between the last newline and the end of the string
				strncpy(server->parse_queue,accumulator,BUFFER_SIZE);
			}else{
				break;
			}
		}
	}
}

//this completes as much as possible until we hit a unique portion (i.e. if there is "ben" and "benjamin" b<tab> should complete to "ben" and give a bell)
//tab completion of names in current channel
//returns the count of unsuccessful tab completions
//note cursor_pos will be re-set after a successful completion
int name_complete(char *input_buffer, int *cursor_pos, int input_display_start, int tab_completions){
	if(current_server>=0){
		irc_connection *server=get_server(current_server);
		
		//a portion of the nickname to complete
		char partial_nick[BUFFER_SIZE];
		//clear out this buffer to start with
		int n;
		for(n=0;n<BUFFER_SIZE;n++){
			partial_nick[n]='\0';
		}
		
		//where the nickname starts
		int partial_nick_start_index=((*cursor_pos)-1);
		while((partial_nick_start_index>=0)&&(input_buffer[partial_nick_start_index]!=' ')){
			partial_nick_start_index--;
		}
		//don't count the delimiter
		partial_nick_start_index++;
		
		//chomp up the nickname start
		for(n=partial_nick_start_index;n<(*cursor_pos);n++){
			partial_nick[n-partial_nick_start_index]=input_buffer[n];
		}
		//always null terminate
		partial_nick[n]='\0';
		
		//lower case for case-insensitive matching
		strtolower(partial_nick,BUFFER_SIZE);
		
		//create a structure to hold ALL matching nicks, so we can do aggregate operations
		dlist_entry *all_matching_nicks=NULL;

		channel_info *ch=(channel_info *)(dlist_get_entry(server->ch,server->current_channel)->data);
		
		//this counts the number of matches we got, we only want to complete on UNIQUE matches
		int matching_nicks=0;
		char last_matching_nick[BUFFER_SIZE];
		//iterate through all tne nicks in this channel, if we find a unique match, complete it
		dlist_entry *nick_entry=ch->user_names;
		for(;nick_entry!=NULL;nick_entry=nick_entry->next){
			nick_info *nick_content=(nick_info *)(nick_entry->data);
			
			char nick_to_match[BUFFER_SIZE];
			strncpy(nick_to_match,nick_content->user_name,BUFFER_SIZE);
			strtolower(nick_to_match,BUFFER_SIZE);
			
			//if this nick started with the partial_nick
			if(strfind(partial_nick,nick_to_match)==0){
				//store in last matching nick
				matching_nicks++;
				strncpy(last_matching_nick,nick_content->user_name,BUFFER_SIZE);
				
				//store in aggregate along with other matching nicks
				nick_info *matching_nick=(nick_info *)(malloc(sizeof(nick_info)));
				strncpy(matching_nick->user_name,nick_content->user_name,BUFFER_SIZE);
				strncpy(matching_nick->mode_str,nick_content->mode_str,BUFFER_SIZE);
				all_matching_nicks=dlist_append(all_matching_nicks,matching_nick);
			}
		}
		
		//if this was a unique match
		if(matching_nicks==1){
			//fill in the rest of the name
			//where to start inserting from in the full nick
			int insert_start_pos=(*cursor_pos)-partial_nick_start_index;
			
			int n;
			for(n=insert_start_pos;n<strlen(last_matching_nick);n++){
				if(strinsert(input_buffer,last_matching_nick[n],(*cursor_pos),BUFFER_SIZE)){
					(*cursor_pos)++;
					//if we would go off the end
					if(((*cursor_pos)-input_display_start)>width){
						//make the end one char further down
						input_display_start++;
					}
				}
			}
			
			//reset the unsuccessful attempt counter
			tab_completions=0;
		//if this wasn't a unique match but we're not out of completion attempts then complete as far as we know how to
		}else if((tab_completions<COMPLETION_ATTEMPTS) && (matching_nicks>1)){
			//the character other possible nicks want to complete next
			char next_char='\0';
			//whether or not we've set that completion char or not
			char next_char_set=FALSE;
			//whether or not all possible completions have the SAME next character
			char agreement=FALSE;
			//how many characters we've added since the point the user tried to complete from
			int chars_inserted=0;
			
			//loop until there is disagreeement on how to complete
			do{
				agreement=FALSE;
				//look through all the possible matches to see what they want to complete to
				dlist_entry *matching_nick_entry=all_matching_nicks;
				for(;matching_nick_entry!=NULL;matching_nick_entry=matching_nick_entry->next){
					//if this nick is longer than what the user typed so far
					int tab_nick_idx=(strlen(partial_nick)+chars_inserted);
					char *matching_nick=(((nick_info *)(matching_nick_entry->data))->user_name);
					
					if(strlen(matching_nick)>tab_nick_idx){
						//if this is the first nick we checked then it gets to say what the next char should be
						if(!next_char_set){
							next_char=matching_nick[tab_nick_idx];
							next_char_set=TRUE;
							agreement=TRUE;
						//if this isn't the first nick and we found a different (competing) completion
						//then there is disagreement!!!
						}else if(next_char!=matching_nick[tab_nick_idx]){
							agreement=FALSE;
							//break out of the loop
							break;
						}
					//if this nick is NOT longer than what the user already has completed then stop. right. now.
					}else{
						//if there was agreement and the partial completion was as long as the nick then two users would have the same name!
						agreement=FALSE;
						//break out of the loop
						break;
					}
				}
				//if everybody agreed then go ahead and type the next character out
				if(agreement){
					if(strinsert(input_buffer,next_char,(*cursor_pos),BUFFER_SIZE)){
						chars_inserted++;
						(*cursor_pos)++;
						//if we would go off the end
						if(((*cursor_pos)-input_display_start)>width){
							//make the end one char further down
							input_display_start++;
						}
					}
				}
				
				next_char_set=FALSE;
				next_char='\0';
			}while(agreement);
			
			//this is still an unsuccesful tab-complete attempt, since the completion wasn't unique
			tab_completions++;
			beep();
		//how many attempts we give the user to complete a name before we just give up and output the possibilities
		}else if(tab_completions>=COMPLETION_ATTEMPTS){
			//the entire line we'll output, we're gonna append to this a lot
			char output_text[BUFFER_SIZE];
			snprintf(output_text,BUFFER_SIZE,"accirc: Attempted to complete %i times in %s; possible completions are: ",tab_completions,ch->name);
			
			//output the array we just made of nicks that are acceptable (but non-unique) completions
			int output_nicks=0;
			dlist_entry *nick_entry=all_matching_nicks;
			for(;nick_entry!=NULL;nick_entry=nick_entry->next){
				nick_info *nick_content=(nick_info *)(nick_entry->data);
				
				//NOTE: source and destination strings cannot be the same for snprintf, which is why a separate temporary buffer is needed
				char tmp_buffer[BUFFER_SIZE];
				if(output_nicks<MAX_OUTPUT_NICKS){
					snprintf(tmp_buffer,BUFFER_SIZE,"%s%s ",output_text,nick_content->user_name);
					strncpy(output_text,tmp_buffer,BUFFER_SIZE);
				}else if(output_nicks==MAX_OUTPUT_NICKS){
					snprintf(tmp_buffer,BUFFER_SIZE,"%s%s",output_text,LINE_OVERFLOW_ERROR);
					strncpy(output_text,tmp_buffer,BUFFER_SIZE);
				}
				output_nicks++;
			}
			scrollback_output(current_server,server->current_channel,output_text,TRUE);
			
			//re-set our completions in case the user was hitting tab like crazy before viewing the output (like I do)
			tab_completions=0;
		//this was not a match
		}else{
			//this was an unsuccessful tab-complete attempt
			tab_completions++;
			beep();
		}
		
		//free the matching nicks array, we're done with it now
		dlist_free(all_matching_nicks,TRUE);
		all_matching_nicks=NULL;
	}
	
	return tab_completions;
}

//C-w bash-style line edit to delete a word from current position to start
void kill_word(char *input_buffer, int *persistent_cursor_pos, int *persistent_input_display_start){
	int cursor_pos=(*persistent_cursor_pos);
	int input_display_start=(*persistent_input_display_start);
	
	while((cursor_pos>0) && (input_buffer[cursor_pos-1]!=' ') && (input_buffer[cursor_pos-1]!='\t')){
		if(strremove(input_buffer,cursor_pos-1)){
			//and update the cursor position upon success
			cursor_pos--;
			if(cursor_pos<input_display_start){
				input_display_start--;
			}
		}
	}
	
	(*persistent_cursor_pos)=cursor_pos;
	(*persistent_input_display_start)=input_display_start;
}

//listen for the next relevant thing to happen and handle it accordingly
//this may be a user input or network read from any connected network
//this is the body of the "main" loop, called from main
void event_poll(int c, char *input_buffer, int *persistent_cursor_pos, int *persistent_input_display_start, int *persistent_tab_completions, time_t *persistent_old_time, char *time_buffer, char *user_status_buffer, char *key_combo_buffer, char *pre_history){
	//make local variables out of the persistent variables from higher scopes
	//note the persistent vars will be re-set to these values at the end of this function
	int cursor_pos=(*persistent_cursor_pos);
	int input_display_start=(*persistent_input_display_start);
	int tab_completions=(*persistent_tab_completions);
	time_t old_time=(*persistent_old_time);
	
	//store the last scrollback position before the event is handled, so we know if an event changed it
	prev_scrollback_end=scrollback_end;
	
	//store what the current_server and channel in that server were previously so we know if they change
	//if they change we'll update the display
	int old_server=current_server;
	int old_channel=-1;
	if(current_server>=0){
		irc_connection *server=get_server(current_server);
		old_channel=server->current_channel;
	}
	
	//store the previous input buffer so we know if it changed
	char old_input_buffer[BUFFER_SIZE];
	strncpy(old_input_buffer,input_buffer,BUFFER_SIZE);
	
	c=-1;
	if(user_input!=NULL){
		c=wgetch(user_input);
	}else if(ncurses_fullscreen_text!=NULL){
		c=wgetch(ncurses_fullscreen_text);
	}
	
	if(c>=0){
		switch(c){
			//handle for resize events
			case KEY_RESIZE:
				//if we were scrolled back we're not anymore!
				scrollback_end=-1;
				force_resize(input_buffer,cursor_pos,input_display_start);
				break;
			//NOTE: f7 now adds ^C=0x03 to the buffer for MIRC colors
			//so ctrl+c /could/ be used for a break command
			//but I'd rather a user not be able to accidentally close the program, so it's not
			
			//handle ctrl+c gracefully
//			case BREAK:
//				done=TRUE;
//				break;
			case KEY_ESCAPE:
				c=wgetch(user_input);
				switch(c){
					case KEY_UP:
					case 'w':
						snprintf(key_combo_buffer,BUFFER_SIZE,"%csl",client_escape);
						parse_input(key_combo_buffer,FALSE);
						break;
					case KEY_DOWN:
					case 'a':
						snprintf(key_combo_buffer,BUFFER_SIZE,"%csr",client_escape);
						parse_input(key_combo_buffer,FALSE);
						break;
					case KEY_LEFT:
					case 's':
						snprintf(key_combo_buffer,BUFFER_SIZE,"%ccl",client_escape);
						parse_input(key_combo_buffer,FALSE);
						break;
					case KEY_RIGHT:
					case 'd':
						snprintf(key_combo_buffer,BUFFER_SIZE,"%ccr",client_escape);
						parse_input(key_combo_buffer,FALSE);
						break;
					//alt+tab is a literal tab character, since tab-completion is done on regular tab
					case '\t':
						if(strinsert(input_buffer,(char)(c),cursor_pos,BUFFER_SIZE)){
							cursor_pos++;
							//if we would go off the end
							if((cursor_pos-input_display_start)>width){
								//make the end one char further down
								input_display_start++;
							}
						}
					//-1 is ERROR, meaning the escape was /just/ an escape and nothing more
					//in that case we want to ignore the handling for subsequent characters
					case -1:
						break;
					//2 escapes in a row should be ignored
					case KEY_ESCAPE:
						break;
					default:
						//this wasn't anything we handle specially for, so just ignore it
						break;
				}
#ifdef DEBUG
				if(current_server<0){
					wblank(channel_text,width,height-RESERVED_LINES);
					wmove(channel_text,0,0);
					wprintw(channel_text,"Handling an escape, c=%i",c);
					wrefresh(channel_text);
				}
#endif
				
				break;
			//NOTE: I now have ALT+arrows bound to move between channels and servers
			//these are the f1,f2,f3, and f4 bindings left for backwards compatibility only
//			case ALT_UP:
			//f3
			case 267:
				snprintf(key_combo_buffer,BUFFER_SIZE,"%csl",client_escape);
				parse_input(key_combo_buffer,FALSE);
				break;
//			case ALT_DOWN:
			//f4
			case 268:
				snprintf(key_combo_buffer,BUFFER_SIZE,"%csr",client_escape);
				parse_input(key_combo_buffer,FALSE);
				break;
//			case ALT_LEFT:
			//f1
			case 265:
				snprintf(key_combo_buffer,BUFFER_SIZE,"%ccl",client_escape);
				parse_input(key_combo_buffer,FALSE);
				break;
//			case ALT_RIGHT:
			//f2
			case 266:
				snprintf(key_combo_buffer,BUFFER_SIZE,"%ccr",client_escape);
				parse_input(key_combo_buffer,FALSE);
				break;
			//user hit enter, meaning parse and handle the user's input
			case '\n':
				//note the input_buffer gets reset to all NULL after parse_input
				parse_input(input_buffer,TRUE);
				//reset the cursor for the next round of input
				cursor_pos=0;
				//and the input display start
				input_display_start=0;
				break;
			//movement within the input line
			case KEY_RIGHT:
				if(cursor_pos<strlen(input_buffer)){
					cursor_pos++;
					if((cursor_pos-input_display_start)>=width){
						input_display_start++;
					}
				}
				refresh_user_input(input_buffer,cursor_pos,input_display_start);
				break;
			case KEY_LEFT:
				if(cursor_pos>0){
					cursor_pos--;
					if((input_display_start>0) && (cursor_pos<input_display_start)){
						input_display_start--;
					}
				}
				refresh_user_input(input_buffer,cursor_pos,input_display_start);
				break;
//			case KEY_PGUP:
			case 339:
				snprintf(key_combo_buffer,BUFFER_SIZE,"%cup",client_escape);
				parse_input(key_combo_buffer,FALSE);
				break;
//			case KEY_PGDN:
			case 338:
				snprintf(key_combo_buffer,BUFFER_SIZE,"%cdown",client_escape);
				parse_input(key_combo_buffer,FALSE);
				break;
			//handle text entry history
			case KEY_UP:
				//reset cursor position always, since the strings in history are probably not the same length as the current input string
				cursor_pos=0;
				input_display_start=0;
				
				if(input_line>0){
					input_line--;
					//actually view that line
					strncpy(input_buffer,input_history[input_line],BUFFER_SIZE);
				//if the user hasn't yet started scrolling into history, start now
				}else if(input_line<0){
					int n;
					//note after this loop n will be one line AFTER the last valid line of scrollback
					for(n=0;(n<MAX_SCROLLBACK)&&(input_history[n]!=NULL);n++);
					//so subtract one
					n--;
					//if there is scrollback to view
					if(n>=0){
						input_line=n;
						//store this line for if we cease viewing history
						strncpy(pre_history,input_buffer,BUFFER_SIZE);
						strncpy(input_buffer,input_history[input_line],BUFFER_SIZE);
					}
				}
				refresh_user_input(input_buffer,cursor_pos,input_display_start);
				break;
			//handle text entry history
			case KEY_DOWN:
				//reset cursor position always, since the strings in history are probably not the same length as the current input string
				cursor_pos=0;
				input_display_start=0;
				
				//if there is valid history below this go there
				if((input_line>=0)&&(input_line<(MAX_SCROLLBACK-1))&&(input_history[input_line+1]!=NULL)){
					input_line++;
					//actually view that line
					strncpy(input_buffer,input_history[input_line],BUFFER_SIZE);
				//otherwise if we're out of history, re-set and make this what the input was before history was viewed
				}else if(input_line>=0){
					strncpy(input_buffer,pre_history,BUFFER_SIZE);
					//note we are now not viewing history
					input_line=-1;
				}
				refresh_user_input(input_buffer,cursor_pos,input_display_start);
				break;
			//handle user name completion
			case '\t':
				tab_completions=name_complete(input_buffer,&cursor_pos,input_display_start,tab_completions);
				break;
			//1 is C-a, added for emacs-style line editing
			case 1:
			case KEY_HOME:
				cursor_pos=0;
				input_display_start=0;
				refresh_user_input(input_buffer,cursor_pos,input_display_start);
				break;
			//5 is C-e, added for emacs-style line editing
			case 5:
			case KEY_END:
				cursor_pos=strlen(input_buffer);
				if(strlen(input_buffer)>width){
					input_display_start=strlen(input_buffer)-width;
				}else{
					input_display_start=0;
				}
				refresh_user_input(input_buffer,cursor_pos,input_display_start);
				break;
			//this accounts for some odd-ness in terminals, it's just backspace (^H)
			case 127:
			//user wants to destroy something they entered
			case KEY_BACKSPACE:
				if(cursor_pos>0){
					if(strremove(input_buffer,cursor_pos-1)){
						//and update the cursor position upon success
						cursor_pos--;
						if(cursor_pos<input_display_start){
							input_display_start--;
						}
					}
				}
				break;
			//user wants to destroy something they entered
			case KEY_DEL:
				if(cursor_pos<strlen(input_buffer)){
					strremove(input_buffer,cursor_pos);
					//note cursor position doesn't change here
				}
				break;
			//11 is C-k, added for emacs-style line editing
			case 11:
				input_buffer[0]='\0';
				cursor_pos=0;
				break;
			//23 is C-w, added for bash and emacs-style line editing
			case 23:
				kill_word(input_buffer,&cursor_pos,&input_display_start);
				break;
			//f5 is refresh left (previous unread, cycle through in order if no unreads remain)
			case 269:
				snprintf(key_combo_buffer,BUFFER_SIZE,"%crefresh left",client_escape);
				parse_input(key_combo_buffer,FALSE);
				break;
			//f6 is refresh (next unread, cycle through in order if no unreads remain)
			case 270:
				snprintf(key_combo_buffer,BUFFER_SIZE,"%crefresh",client_escape);
				parse_input(key_combo_buffer,FALSE);
				break;
			//NOTE: this is alt+tab also, f5 left here only for backwards compatibility
			//f9 sends a literal tab
			case 273:
			//and f10 sends a 0x01 (since screen catches the real one)
			case 274:
			//f11 is a literal 0x03 for mirc color sending
			case 275:
				//these are mutually exclusive, so an if is needed
				if(c==273){
					c='\t';
				}else if(c==274){
					c=0x01;
				}else if(c==275){
					c=0x03;
				}
			//normal input
			default:
				if(strinsert(input_buffer,(char)(c),cursor_pos,BUFFER_SIZE)){
					cursor_pos++;
					//if we would go off the end
					if((cursor_pos-input_display_start)>width){
						//make the end one char further down
						input_display_start++;
					}
				}
				
				//reset the unsuccessful tab attempt counter
				tab_completions=0;
				
#ifdef DEBUG
				if(current_server<0){
					wblank(channel_text,width,height-RESERVED_LINES);
					wmove(channel_text,0,0);
					wprintw(channel_text,"%i",c);
					wrefresh(channel_text);
				}
#endif
				break;
		}
	}
	
	//look for new data on all connected servers; if some is found, handle it!
	read_server_data();
	
	//refresh the bottom bar above the input area
	//this refresh once a second or when the display changes
	refresh_statusbar(&old_time,time_buffer,user_status_buffer);
	
	//output the most up-to-date information about servers, channels, topics, and various whatnot
	//(do this where changes occur so we're not CONSTANTLY refreshing, which causes flicker among other things)
	if((old_server!=current_server)&&(current_server>=0)){
		refresh_server_list();
		refresh_channel_list();
		refresh_channel_topic();
		refresh_channel_text();
	//if the server didn't change but the channel did, still update the channel text
	}else if((current_server>=0)&&(old_channel!=(get_current_channel(current_server)))){
		refresh_channel_list();
		refresh_channel_topic();
		refresh_channel_text();
	}
	
	//the cursor is one thing that should ALWAYS be visible
	wmove(user_input,0,cursor_pos);
	
	//if the user typed something
	if(strncmp(input_buffer,old_input_buffer,BUFFER_SIZE)!=0){
		//if the string is not as wide as the display allows re-set the input display starting point to show the whole string always
		if(strlen(input_buffer)<width){
			input_display_start=0;
		}
		refresh_user_input(input_buffer,cursor_pos,input_display_start);
	}
	
	//update persistent variables (pointers) for the next iteration of the event polling loop
	//these are side-effects because C; in other languages they should have maybe been a list that got returned
	//anyway, consider this essentially a set of return values
	(*persistent_cursor_pos)=cursor_pos;
	(*persistent_input_display_start)=input_display_start;
	(*persistent_tab_completions)=tab_completions;
	(*persistent_old_time)=old_time;
}

//runtime
int main(int argc, char *argv[]){
	//generic loop counter variable
	int n;
	
	error_file=NULL;
	ignore_rc=FALSE;
	easy_mode=TRUE;
	center_server_list=TRUE;
#ifdef _OPENSSL
	ssl_cert_check=TRUE;
#endif
	//by default PM faux-channels are opened as needed
	auto_hi=TRUE;
	char rc_file[BUFFER_SIZE];
	strncpy(rc_file,"",BUFFER_SIZE);
	
	//support utf-8
	setlocale(LC_CTYPE,"C.UTF-8");
//	setlocale(LC_ALL, "");
	
	//handle special argument cases like --version, --help, etc.
	if(argc>1){
		if(!strcmp(argv[1],"--version")){
			printf("accidental_irc, the accidental irc client, version %s compiled %s %s\n",VERSION,__DATE__,__TIME__);
			exit(0);
		}else if(!strcmp(argv[1],"--help")){
			printf("accidental_irc, the irc client that accidentally got written; see man page for docs\n");
			printf("a short summary of commands is as follows, but detailed docs and documentation for additional commands are in the man page\n");
			int n;
			for(n=0;n<(sizeof(command_list)/sizeof(command_list[0]));n++){
				printf("accirc: command: %s\n",command_list[n]);
			}
			exit(0);
		}
		
		//handle runtime arguments that will persist during execution
		int n;
		for(n=1;n<argc;n++){
			if((!strcmp(argv[n],"--ignorerc")) || (!strcmp(argv[n],"--ignore-rc"))){
				ignore_rc=TRUE;
			}else if(!strcmp(argv[n],"--proper")){
				easy_mode=FALSE;
#ifdef _OPENSSL
			}else if(!strcmp(argv[n],"--no-ssl-cert-check")){
				ssl_cert_check=FALSE;
#endif
			//allow for a custom rc file path to be passed
			//NOTE: the rc file path must be provided as an argument after this or this gets ignored
			}else if(((!strcmp(argv[n],"--rc")) || (!strcmp(argv[n],"--rc-file"))) && (n+1<argc)){
				strncpy(rc_file,argv[n+1],BUFFER_SIZE);
			}
		}
	}
	
	//store error log in ~/.local/share/accirc/error_log.txt
	//ensure appropriate directories exist for config and logs
	char error_file_buffer[BUFFER_SIZE];
	char *home_dir=getenv("HOME");
	if(home_dir!=NULL){
		snprintf(error_file_buffer,BUFFER_SIZE,"%s/.local/share/accirc/%s",home_dir,ERROR_FILE);
		verify_or_make_parent_dirs_for(error_file_buffer);
	}else{
		snprintf(error_file_buffer,BUFFER_SIZE,ERROR_FILE);
	}
	
	//try to make an error log file, if that's impossible just use stderr
	error_file=fopen(error_file_buffer,"a");
	if(error_file==NULL){
		fprintf(stderr,"Err: Could not find or create the error log file\n");
		//fall back to stderr
		error_file=stderr;
	}
	
	//turn off buffering since I need may this output immediately and buffers annoy me for that
	setvbuf(error_file,NULL,_IONBF,0);
	
	//by default you can log stuff
	//NOTE: this is re-used for logging pings also, which are considered just a special case of logging
	can_log=TRUE;
	
	//store logs in ~/.local/share/accirc/logs/
	//ensure appropriate directories exist for config and logs
	char log_dir[BUFFER_SIZE];
	if(home_dir!=NULL){
		snprintf(log_dir,BUFFER_SIZE,"%s/.local/share/accirc/%s",home_dir,LOGGING_DIRECTORY);
		verify_or_make_parent_dirs_for(log_dir);
	}else{
		snprintf(log_dir,BUFFER_SIZE,LOGGING_DIRECTORY);
	}
	
	//not making log dir is a non-fatal error
	if(!verify_or_make_dir(log_dir)){
		error_log("Warn: Could not find or create the log directory");
		can_log=FALSE;
	}
	
	//register signal handlers
	if(signal(SIGHUP,terminal_close)==SIG_ERR){
		error_log("Warn: Could not register the SIGHUP signal handler (closing terminal will time out)");
	}
	
	//initialize the global variables appropriately
	servers=NULL;
	
	//by default the time format is unix time, this can be changed with the "time_format" client command
	snprintf(time_format,BUFFER_SIZE,"%%s");
	//by default the software CTCP version string is the real version of the software
	snprintf(custom_version,BUFFER_SIZE,"accidental_irc v%s compiled %s %s",VERSION,__DATE__,__TIME__);
	//clear out ping phrase list (the in-use nickname is always considered a ping though)
	ping_phrases=NULL;
	//and clear out the alias list
	alias_list=NULL;
	
	//location in input history, starting at "not looking at history" state
	input_line=-1;
	//location in scrollback for the current channel, -1 meaning not scrolled back at all (and hence new messages are displayed as they come in)
	scrollback_end=-1;
	
	//negative values mean the user is not connected to anything (this is the default)
	current_server=-1;
	
	//declare some variables
	//for the clock
	char time_buffer[BUFFER_SIZE];
	//for the user count display on the status bar
	char user_status_buffer[BUFFER_SIZE];
	//for user input
	char input_buffer[BUFFER_SIZE];
	for(n=0;n<BUFFER_SIZE;n++){
		time_buffer[n]='\0';
		user_status_buffer[n]='\0';
		input_buffer[n]='\0';
	}
	
	//start ncurses interface
	initscr();
	
	//allocate windows for our toolbars and the main chat
	server_list=NULL;
	channel_list=NULL;
	channel_topic=NULL;
	top_border=NULL;
	channel_text=NULL;
	bottom_border=NULL;
	user_input=NULL;
	
	//in case the window is too small to display irc
	//allocate a buffer for an error message to show
	ncurses_fullscreen_text=NULL;
	
	//force a re-detection of the window and a re-allocation of resources
	force_resize("",0,0);
	
	//store config in ~/.config/accirc/config.rc unless otherwise specified
	if(strlen(rc_file)<1){
		if(home_dir!=NULL){
			snprintf(rc_file,BUFFER_SIZE,"%s/.config/accirc/config.rc",home_dir);
			verify_or_make_parent_dirs_for(rc_file);
		}else{
			snprintf(rc_file,BUFFER_SIZE,"config.rc");
		}
	}
	
	//until we're connected to a server we can't listen for post commands
	post_listen=FALSE;
	
	char easy_mode_buf[BUFFER_SIZE];

	//for all users set a few general command aliases just to be more user-friendly
	//(you can un-set these if you want)
	
	snprintf(easy_mode_buf,BUFFER_SIZE,"%calias use_mode_str %cmode_str",client_escape,client_escape);
	parse_input(easy_mode_buf,FALSE);

	snprintf(easy_mode_buf,BUFFER_SIZE,"%calias show_mode_str %cmode_str",client_escape,client_escape);
	parse_input(easy_mode_buf,FALSE);
	
	//if we're not being proper, then register some default aliases and set more friendly settings
	//this is to make it easier for non-experts to use this client (mostly for the kindle port)
	//this is done before the rc loading so the aliases are available to crappy rc files
	if(easy_mode){
		snprintf(easy_mode_buf,BUFFER_SIZE,"%calias nick %cnick",client_escape,server_escape);
		parse_input(easy_mode_buf,FALSE);
		
		snprintf(easy_mode_buf,BUFFER_SIZE,"%calias join %cjoin",client_escape,server_escape);
		parse_input(easy_mode_buf,FALSE);
		
		snprintf(easy_mode_buf,BUFFER_SIZE,"%calias part %cpart",client_escape,server_escape);
		parse_input(easy_mode_buf,FALSE);
		
		snprintf(easy_mode_buf,BUFFER_SIZE,"%calias quit %cexit",client_escape,client_escape);
		parse_input(easy_mode_buf,FALSE);
		
		snprintf(easy_mode_buf,BUFFER_SIZE,"%calias msg %chi",client_escape,client_escape);
		parse_input(easy_mode_buf,FALSE);
		
		snprintf(easy_mode_buf,BUFFER_SIZE,"%ctime_format %%Y-%%m-%%d %%R:%%S",client_escape);
		parse_input(easy_mode_buf,FALSE);
	}
	
	//unless we've been explicitly asked not to, try to load the rc file
	if(!ignore_rc){
		//if this fails no rc will be used
		char rc_success=load_rc(rc_file);
		if(!rc_success){
#ifdef DEBUG
			error_log("Err: Could not load rc file");
#endif
		}
	}
	
	//start the clock
	time_t old_time=time(NULL);
	
	//start the cursor in the user input area
	wmove(user_input,0,0);
	wrefresh(user_input);
	
	//a one-buffer long history for when the user is scrolling back into the real history
	//heh, pre-history; this is the dino-buffer, :P
	char pre_history[BUFFER_SIZE];
	
	//this is a buffer for key combinations, since they are really just commands that get bound to keys
	//so this sends them as a command to parse_input, then handles it as if it were typed in
	char key_combo_buffer[BUFFER_SIZE];
	
	//the current position in the string, starting at 0
	int cursor_pos=0;
	//where in the string to start displaying (needed when the string being input is larger than the width of the window)
	int input_display_start=0;
	//one character of input
	int c=0;
	//how many unsuccessful tab completions we've had since the last successful one or non-tab character
	int tab_completions=0;
	//determine if we're done
	done=FALSE;
	
	//MAIN LOOP, everything between initialization and shutdown is HERE
	while(!done){
		//EVENT POLLING (tons of side-effects here, which is why we're passing a bunch of pointers)
		event_poll(c,input_buffer,&cursor_pos,&input_display_start,&tab_completions,&old_time,time_buffer,user_status_buffer,key_combo_buffer,pre_history);
	}
	
	//now that we're done, close the error log file
	fclose(error_file);
	
	//free all the RAM we allocated for anything
	while(dlist_length(servers)>0){
		properly_close(0);
	}
	
	//free the ping phrase list
	dlist_free(ping_phrases,TRUE);
	ping_phrases=NULL;
	
	//end ncurses cleanly
	endwin();
	
	return 0;
}

