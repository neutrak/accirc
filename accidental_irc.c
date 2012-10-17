//accidental irc, the accidental mutlti-server ncurses irc client
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
//ncurses
#include <ncurses.h>
//networking
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
//non-blocking
#include <fcntl.h>
#include <errno.h>
//time
#include <sys/time.h>
#include <time.h>

//preprocessor defines
#define TRUE 1
#define FALSE 0

//the smallest terminal we can actually use (smaller than this and we exit with error)
#define MIN_HEIGHT 7
#define MIN_WIDTH 12

#define VERSION "0.1"

//these are for ncurses' benefit
#define KEY_ESCAPE 0x1b
#define KEY_DEL KEY_DC
#define BREAK 0x03
//(I know the IRC spec limits it to 512 but I'd like to have some extra room in case of client commands or something)
#define BUFFER_SIZE 1024
//who's gonna be on more than 64 servers at once? really?
#define MAX_SERVERS 64
//same for channels
#define MAX_CHANNELS 64
//the number of lines of scrollback to store (per channel, and for input history) (each line being BUFFER_SIZE chars long)
#define MAX_SCROLLBACK 1000
//maximum number of users in a channel
#define MAX_NAMES 200

//for MIRC colors (these are indexes in an array)
#define FOREGROUND 0
#define BACKGROUND 1

//some defaults in case the user forgets to give a nickname
#define DEFAULT_NICK "accidental_irc_user"

//and in case the user doesn't give a proper quit message
#define DEFAULT_QUIT_MESSAGE "accidental_irc exited"

//for reconnecting we should re-send user information, it'll use this
#define DEFAULT_USER "1 2 3 4"

//the error to display when a line overflows
#define LINE_OVERFLOW_ERROR "<<etc.>>"

//the default directoryto save logs to
#define LOGGING_DIRECTORY "logs"

//the number of seconds to try to reconnect before giving up
#define RECONNECT_TIMEOUT 2

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

//global variables
typedef struct irc_connection irc_connection;
struct irc_connection {
	//behind-the-scenes data, the user never sees it
	int socket_fd;
	char read_buffer[BUFFER_SIZE];
	//this data is stored in case the connection dies and we need to re-connect
	int port;
	//tells us if there is new content on this server since the user last viewed it
	char new_server_content;
	//logging information
	char keep_logs;
	FILE *log_file[MAX_CHANNELS];
	//the channels we're waiting to join when ready
	char *autojoin_channel[MAX_CHANNELS];
	//the password to give NickServ once connected
	char ident[BUFFER_SIZE];
	//whether, on this server, to rejoin channels when kicked
	char rejoin_on_kick;
	//the nick to use if the user's nick is already in use
	char fallback_nick[BUFFER_SIZE];
	//whether to try to reconnect to this server if connection is lost
	char reconnect;
	
	//this data is what the user sees (but is also used for other things)
	char server_name[BUFFER_SIZE];
	char nick[BUFFER_SIZE];
	char *channel_name[MAX_CHANNELS];
	char **channel_content[MAX_CHANNELS];
	char **user_names[MAX_NAMES];
	char *channel_topic[MAX_CHANNELS];
	//this is a flag to tell if there's new content in a channel
	char new_channel_content[MAX_CHANNELS];
	int current_channel;
};

int current_server;
irc_connection *servers[MAX_SERVERS];
//a history of lines the user has input so you can up and down arrow through them
char *input_history[MAX_SCROLLBACK];
//the location in input_history we're at now
int input_line;
//the location in the scrollback for the current channel we're at now
//(this var stores the index of the LAST line to output, hence "end")
int scrollback_end;

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

//forward declarations (trying to be minimal with these)
void scrollback_output(int server_index, int output_channel, char *to_output);
void parse_input(char *input_buffer, char keep_history);
void refresh_server_list();
void refresh_channel_list();
void refresh_channel_topic();
void refresh_channel_text();

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

//load the configuration file at path, returns TRUE on success, FALSE on failure
char load_rc(char *rc_file){
	FILE *rc=fopen(rc_file,"r");
	if(!rc){
//		fprintf(stderr,"Warn: rc file not found, not executing anything on startup\n");
		return FALSE;
	}else{
		//read in the .rc, parse_input each line until the end
		char rc_line[BUFFER_SIZE];
		while(!feof(rc)){
			fgets(rc_line,BUFFER_SIZE,rc);
			if(!feof(rc)){
				//cut off the trailing newline
				int newline_index=strfind("\n",rc_line);
				if(newline_index>=0){
					substr(rc_line,rc_line,0,newline_index);
				}
				parse_input(rc_line,FALSE);
				
				refresh_server_list();
				refresh_channel_list();
				refresh_channel_topic();
				refresh_channel_text();
			}
		}
		
		fclose(rc);
		return TRUE;
	}
	return TRUE;
}

void wcoloron(WINDOW *win, int fg, int bg){
	wattron(win,COLOR_PAIR((fg<<4)|(bg<<0)));
}

void wcoloroff(WINDOW *win, int fg, int bg){
	wattroff(win,COLOR_PAIR((fg<<4)|(bg<<0)));
}

//returns TRUE if successful
//FALSE on error
char safe_send(int socket, char *buffer){
	//properly terminate the buffer just in case
	buffer[BUFFER_SIZE-1]='\0';
	if(send(socket,buffer,strlen(buffer),0)<0){
//		fprintf(stderr,"Err: Could not send data\n");
		return FALSE;
	}
	return TRUE;
}

//returns TRUE if successful
//FALSE on error
char safe_recv(int socket, char *buffer){
	int bytes_transferred=recv(socket,buffer,BUFFER_SIZE,0);
	if(bytes_transferred<=0){
//		fprintf(stderr,"Err: Could not receive data\n");
		return FALSE;
	}
	//clear out the remainder of the buffer just in case
	int n;
	for(n=bytes_transferred;n<BUFFER_SIZE;n++){
		buffer[n]='\0';
	}
	return TRUE;
}

void properly_close(int server_index){
	//if we're not connected in the first place just leave, we're done here
	if(server_index<0){
		return;
	}
	
	//if we want to reconnect to this server handle that
	char reconnect_this=servers[server_index]->reconnect;
	
	char reconnect_host[BUFFER_SIZE];
	int reconnect_port;
	char *reconnect_channels[MAX_CHANNELS];
	char reconnect_nick[BUFFER_SIZE];
	char reconnect_ident[BUFFER_SIZE];
	//if we'll be reconnecting to this server
	if(reconnect_this){
		//remember all the necessary information in order to reconnect
		strncpy(reconnect_host,servers[server_index]->server_name,BUFFER_SIZE);
		reconnect_port=servers[server_index]->port;
		
		//we'll automatically rejoin the channels we were already in
		int n;
		reconnect_channels[0]=NULL; //never reconnect to the reserved system channel
		for(n=1;n<MAX_CHANNELS;n++){
			if(servers[server_index]->channel_name[n]!=NULL){
				reconnect_channels[n]=malloc(BUFFER_SIZE*sizeof(char));
				strncpy(reconnect_channels[n],servers[server_index]->channel_name[n],BUFFER_SIZE);
			}else{
				reconnect_channels[n]=NULL;
			}
		}
		
		strncpy(reconnect_nick,servers[server_index]->nick,BUFFER_SIZE);
		strncpy(reconnect_ident,servers[server_index]->ident,BUFFER_SIZE);
	}
	
	//clean up the server
	close(servers[server_index]->socket_fd);
	//free RAM null this sucker out
	int n;
	for(n=0;n<MAX_CHANNELS;n++){
		if(servers[server_index]->channel_name[n]!=NULL){
			free(servers[server_index]->channel_name[n]);
			free(servers[server_index]->channel_topic[n]);
			free(servers[server_index]->autojoin_channel[n]);
			
			int n1;
			for(n1=0;n1<MAX_SCROLLBACK;n1++){
				if(servers[server_index]->channel_content[n][n1]!=NULL){
					free(servers[server_index]->channel_content[n][n1]);
				}
			}
			free(servers[server_index]->channel_content[n]);
			
			if(servers[server_index]->log_file[n]!=NULL){
				fclose(servers[server_index]->log_file[n]);
			}
			
			for(n1=0;n1<MAX_NAMES;n1++){
				if(servers[server_index]->user_names[n][n1]!=NULL){
					free(servers[server_index]->user_names[n][n1]);
				}
			}
			free(servers[server_index]->user_names[n]);
		}
	}
					
	free(servers[server_index]);
	servers[server_index]=NULL;
	
	//set a new current_server if we were on that one
	if(current_server==server_index){
		current_server=-1;
		int n;
		for(n=0;n<MAX_SERVERS;n++){
			if(servers[n]!=NULL){
				current_server=n;
			}
		}
	}
	
	//if we'll be reconnecting to this server
	if(reconnect_this){
		char reconnect_command[BUFFER_SIZE];
		sprintf(reconnect_command,"/connect %s %i",reconnect_host,reconnect_port);
		
		//find where the next connection will go so we know if the command was successful
		int next_server;
		for(next_server=0;(next_server<MAX_SERVERS)&&(servers[next_server]!=NULL);next_server++);
		
		if(next_server<MAX_SERVERS){
			//try to reconnect again and again until we're either connected or we timed out
			char timeout=FALSE;
			uintmax_t start_time=time(NULL);
			while((!timeout)&&(servers[next_server]==NULL)){
				uintmax_t current_time=time(NULL);
				if((current_time-start_time)>RECONNECT_TIMEOUT){
					timeout=TRUE;
				}else{
					//send the connect command (if this works then servers[next_server] should no longer be NULL, breaking the loop)
					parse_input(reconnect_command,FALSE);
				}
			}
			
			//because there wasn't a timeout, implicitly we successfully connected
			if(!timeout){
				int old_server;
				if(current_server>=0){
					old_server=current_server;
				}else{
					old_server=next_server;
				}
				
				//we're connected, so time to do some stuff
				current_server=next_server;
				
				char command_buffer[BUFFER_SIZE];
				sprintf(command_buffer,":nick %s",reconnect_nick);
				parse_input(command_buffer,FALSE);
				sprintf(command_buffer,":user %s",DEFAULT_USER);
				parse_input(command_buffer,FALSE);
				if(strcmp(reconnect_ident,"")!=0){
					sprintf(command_buffer,"/autoident %s",reconnect_ident);
					parse_input(command_buffer,FALSE);
				}
				
				//rejoin the channels we were in when possible
				int n;
				for(n=0;n<MAX_CHANNELS;n++){
					if(reconnect_channels[n]!=NULL){
						sprintf(command_buffer,"/autojoin %s",reconnect_channels[n]);
						parse_input(command_buffer,FALSE);
						//free the memory
						free(reconnect_channels[n]);
						reconnect_channels[n]=NULL;
					}
				}
				
				//if you wanted to reconnect before you probably still want to
				//this segfaults with parse_input but doesn't fail at all with just setting the value, not sure why that is
//				parse_input("/reconnect",FALSE);
				servers[next_server]->reconnect=TRUE;
				
				//go back to whatever server you were on
				current_server=old_server;
			}
			//else it was a timeout, so sorry, can't do anything, give up
		}
	}
	
	//output
	if(current_server<0){
		wclear(server_list);
		wclear(channel_list);
		wclear(channel_topic);
		wclear(channel_text);
		
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
}

//display server list updates as needed; bolding the current server
void refresh_server_list(){
	//if we're not connected to anything don't bother
	if(current_server<0){
		return;
	}
	
	//update the display of the server list
	wclear(server_list);
	wmove(server_list,0,0);
	int n;
	for(n=0;n<MAX_SERVERS;n++){
		//if the server is connected
		if(servers[n]!=NULL){
			//set the new server content to be the OR of all channels on that server
			servers[n]->new_server_content=FALSE;
			int n1;
			for(n1=0;n1<MAX_CHANNELS;n1++){
				if(servers[n]->channel_name[n1]!=NULL){
					servers[n]->new_server_content=((servers[n]->new_server_content)||(servers[n]->new_channel_content[n1]));
				}
			}
			
			//if it's the active server bold it
			if(current_server==n){
				wattron(server_list,A_BOLD);
				wprintw(server_list,servers[n]->server_name);
				wattroff(server_list,A_BOLD);
				
				//if we're viewing this server any content that would be considered "new" is no longer there
				servers[current_server]->new_server_content=FALSE;
			//else if there is new data on this server we're currently iterating on, display differently to show that to the user
			}else if(servers[n]->new_server_content==TRUE){
				wattron(server_list,A_UNDERLINE);
				wprintw(server_list,servers[n]->server_name);
				wattroff(server_list,A_UNDERLINE);
			//otherwise just display it regularly
			}else{
				wprintw(server_list,servers[n]->server_name);
			}
			
			//and display the nickname we're using for that server
			if(strcmp(servers[n]->nick,"")!=0){
				wprintw(server_list," (");
				wprintw(server_list,servers[n]->nick);
				wprintw(server_list,")");
			}
			
			//add a delimeter for formatting purposes
			wprintw(server_list," | ");
		}
	}
	wrefresh(server_list);
}

//display channel list updates as needed; bolding the current channel
void refresh_channel_list(){
	//if we're not connected to anything don't bother
	if(current_server<0){
		return;
	}
	
	//update the display of the channel list
	wclear(channel_list);
	wmove(channel_list,0,0);
	int n;
	for(n=0;n<MAX_CHANNELS;n++){
		//if the server is connected
		if(servers[current_server]->channel_name[n]!=NULL){
			//if it's the active server bold it
			if(servers[current_server]->current_channel==n){
				wattron(channel_list,A_BOLD);
				wprintw(channel_list,servers[current_server]->channel_name[n]);
				wattroff(channel_list,A_BOLD);
				
				//if we're viewing this channel any content that would be considered "new" is no longer there
				servers[current_server]->new_channel_content[n]=FALSE;
			//else if there is new data, display differently to show that to the user
			}else if(servers[current_server]->new_channel_content[n]==TRUE){
				wattron(channel_list,A_UNDERLINE);
				wprintw(channel_list,servers[current_server]->channel_name[n]);
				wattroff(channel_list,A_UNDERLINE);
			//otherwise just display it regularly
			}else{
				wprintw(channel_list,servers[current_server]->channel_name[n]);
			}
			
			//add a delimeter for formatting purposes
			wprintw(channel_list," | ");
		}
	}
	wrefresh(channel_list);
}

//display channel topic as needed 
void refresh_channel_topic(){
	//if we're not connected to anything don't bother
	if(current_server<0){
		return;
	}
	
	//print out the channel topic
	//first clearing that window
	wclear(channel_topic);
	
	//start at the start of the line
	wmove(channel_topic,0,0);
	char topic[BUFFER_SIZE];
	strncpy(topic,servers[current_server]->channel_topic[servers[current_server]->current_channel],BUFFER_SIZE);
	if(strlen(topic)<width){
		wprintw(channel_topic,topic);
	}else{
		//NOTE: although we're not outputting the full line here, the full line WILL be in the logs for the user to view
		//and WILL be in the server information should the user resize the window
		int n;
		for(n=0;n<width;n++){
			topic[n]=servers[current_server]->channel_topic[servers[current_server]->current_channel][n];
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
	//if we're not connected to anything don't bother
	if(current_server<0){
		return;
	}
	
	//number of messages in scrollback available
	int message_count;
	char **scrollback=servers[current_server]->channel_content[servers[current_server]->current_channel];
	for(message_count=0;(message_count<MAX_SCROLLBACK)&&(scrollback[message_count]!=NULL);message_count++);
	
	int w_height,w_width;
	getmaxyx(channel_text,w_height,w_width);
	
	//print out the channel text
	//first clearing that window
	wclear(channel_text);
	
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
			
//			if(has_colors()){
/*
				//TODO: handle MIRC colors
				//if this line was a ping or included MIRC colors treat it specially (set attributes before output)
				
				char was_ping=FALSE;
				
				char find_buffer[BUFFER_SIZE];
				sprintf(find_buffer,"%s",servers[current_server]->nick);
				int ping_check=strfind(find_buffer,scrollback[output_line]);
				//if our name was in the message and we didn't send the message and it's not an unhandled message type (those start with ":")
				if((ping_check>=0)&&(strfind(">>",scrollback[output_line])!=0)&&(strfind(":",scrollback[output_line])!=0)){
					was_ping=TRUE;
				}
				
				if(was_ping){
//					wcoloron(channel_text,MIRC_GREEN,MIRC_BLACK);
					wcoloron(channel_text,2,0);
				}
				
				//output the string a character at a time, taking into consideration MIRC colors
				int n;
				for(n=0;n<strlen(output_text);n++){
					if(output_text[n]!=0x03){
						wprintw(channel_text,"%c",output_text[n]);
					}else{
						n++;
						int color_start=n;
						
						int colors[2];
						colors[FOREGROUND]=0;
						colors[BACKGROUND]=0;
						
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
						//and decrement n because the next character is something we want to display as a normal char
						n--;
						
						//if not one iteration of the loop was successful this is a reset escape, so reset
						if(color_start==n){
							wattrset(channel_text,0);
							wcoloron(channel_text,0,1);
						}else{
							if((colors[FOREGROUND]>=0)&&(colors[FOREGROUND]<MIRC_COLOR_MAX)&&(colors[BACKGROUND]>=0)&&(colors[BACKGROUND]<MIRC_COLOR_MAX)){
								//okay, we know what we're setting now so set it and display
								wcoloron(channel_text,colors[FOREGROUND],colors[BACKGROUND]);
								wprintw(channel_text,"%c",output_text[n]);
								wcoloroff(channel_text,colors[FOREGROUND],colors[BACKGROUND]);
							}else{
								wprintw(channel_text,"%c",output_text[n]);
							}
						}
					}
				}
				
				if(was_ping){
//					wcoloroff(channel_text,MIRC_GREEN,MIRC_BLACK);
					wcoloroff(channel_text,2,0);
				}
				
				//reset all attributes before we start outputting the next line in case they didn't properly terminate their colors
//				wattrset(channel_text,0);
*/
//			}else{
//				wprintw(channel_text,output_text);
				
				//instead of a line overflow error, WRAP! (this is a straight-up character wrap)
				int wrapped_line=0;
//				int n;
				for(n=0;n<strlen(output_text);n++){
					wprintw(channel_text,"%c",output_text[n]);
					if(((n+1)<strlen(output_text))&&((n+1)%width==0)){
						wrapped_line++;
						wmove(channel_text,(y_start+wrapped_line),0);
					}
				}
//			}
		}
	}
	//refresh the channel text window
	wrefresh(channel_text);
}

//refresh the user's input, duh
void refresh_user_input(char *input_buffer, int cursor_pos, int input_display_start){
	//output the most recent text from the user so they can see what they're typing
	wclear(user_input);
	wmove(user_input,0,0);
	
	//if we can output the whole string just do that no matter what
	int length=strlen(input_buffer);
	if(length<width){
		input_display_start=0;
	}
	
	int n;
	for(n=input_display_start;(n<(input_display_start+width))&&(n<BUFFER_SIZE);n++){
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

void scrollback_output(int server_index, int output_channel, char *to_output){
	char output_buffer[BUFFER_SIZE];
	strncpy(output_buffer,to_output,BUFFER_SIZE);
	
	//regardless of what our output was, timestamp it
	char time_buffer[BUFFER_SIZE];
	sprintf(time_buffer,"%lu %s",(uintmax_t)(time(NULL)),output_buffer);
	strncpy(output_buffer,time_buffer,BUFFER_SIZE);
	
	//add the message to the relevant channel scrollback structure
	char **scrollback=servers[server_index]->channel_content[output_channel];
	
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
//	strncpy(scrollback[scrollback_line],servers[server_index]->read_buffer,BUFFER_SIZE);
	strncpy(scrollback[scrollback_line],output_buffer,BUFFER_SIZE);
	
	//indicate that there is new text if the user is not currently in this channel
	//through the channel_list
	servers[server_index]->new_channel_content[output_channel]=TRUE;
	refresh_channel_list();
	
	//if we're keeping logs write to them
	if((servers[server_index]->keep_logs)&&(servers[server_index]->log_file[output_channel]!=NULL)){
		fprintf(servers[server_index]->log_file[output_channel],"%s\n",output_buffer);
	}
	
	//if this was currently in view update it there
	if((current_server==server_index)&&(servers[server_index]->current_channel==output_channel)){
		refresh_channel_text();
	}
}

void leave_channel(int server_index, char *ch){
	char channel[BUFFER_SIZE];
	strncpy(channel,ch,BUFFER_SIZE);
	strtolower(channel,BUFFER_SIZE);
	
	//go through the channels, find out the one to remove from our structures
	//note that we start at 1, 0 is always the reserved server channel
	int channel_index;
	for(channel_index=1;channel_index<MAX_CHANNELS;channel_index++){
		if(servers[server_index]->channel_name[channel_index]!=NULL){
			char lower_case_channel[BUFFER_SIZE];
			strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
			strtolower(lower_case_channel,BUFFER_SIZE);
			
			if(!strcmp(channel,lower_case_channel)){
				//free any associated RAM, and NULL those pointers
				free(servers[server_index]->channel_name[channel_index]);
				servers[server_index]->channel_name[channel_index]=NULL;
				free(servers[server_index]->channel_topic[channel_index]);
				servers[server_index]->channel_topic[channel_index]=NULL;
				
				int n;
				for(n=0;n<MAX_SCROLLBACK;n++){
					if(servers[server_index]->channel_content[channel_index][n]!=NULL){
						free(servers[server_index]->channel_content[channel_index][n]);
						servers[server_index]->channel_content[channel_index][n]=NULL;
					}
				}
				
				//if we were keeping logs close them
				if((servers[server_index]->keep_logs)&&(servers[server_index]->log_file[channel_index]!=NULL)){
					fclose(servers[server_index]->log_file[channel_index]);
					servers[server_index]->log_file[channel_index]=NULL;
				}
				
				//if we were in this channel kick back to the reserved SERVER channel
				if(servers[server_index]->current_channel==channel_index){
					servers[server_index]->current_channel=0;
				}
				
				//and refresh the channel list
				refresh_channel_list();
			}
		}
	}
}

//parse user's input (note this is conextual based on current server and channel)
//because some input may be given recursively or from key bindings, there is a history flag to tell if we should actually consider this input in the history
void parse_input(char *input_buffer, char keep_history){
	//go to the end of scrollback because why would the user input something and not want to see it?
	scrollback_end=-1;
	
	//ignore blank commands
	if(!strcmp("",input_buffer)){
		goto fail;
	}
	
	if(keep_history){
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
	
	//flags to tell if this is any kind of command
	char server_command=FALSE;
	char client_command=FALSE;
	
	//TODO: make the client and server command escapes a setting
	if((input_buffer[0]==':')||(input_buffer[0]=='/')||(input_buffer[0]=='\\')){
		//server command
		if(input_buffer[0]==':'){
			server_command=TRUE;
			client_command=FALSE;
		//client command	
		}else if(input_buffer[0]=='/'){
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
		//connect to a server
		if(!strcmp("connect",command)){
			char host[BUFFER_SIZE];
			char port_buffer[BUFFER_SIZE];
			int first_space=strfind(" ",parameters);
			if(first_space<0){
				//handle for insufficient parameters
				//note text output cannot be done here (except maybe to stdout or stderr) since we not be connected to any server
				int n;
				for(n=0;n<3;n++){
					beep();
					usleep(100000);
				}
			}else{
				substr(host,parameters,0,first_space);
				substr(port_buffer,parameters,first_space+1,strlen(parameters)-(first_space+1));
				int port=atoi(port_buffer);
				
				//handle a new connection
				//make a socket file descriptor
				int new_socket_fd;
				
				//the address of the server
				struct sockaddr_in serv_addr;
				
				//set the socket file descriptor
				//arguments mean ipv4, stream type, and tcp protocol
				new_socket_fd=socket(AF_INET,SOCK_STREAM,0);
				
				if(new_socket_fd<0){
					//TODO: handle failed socket openings gracefully here
#ifdef DEBUG
//					fprintf(stderr,"Err: Could not open socket\n");
#endif
					goto fail;
				}
				
				//that's "get host by name"
				struct hostent *server=gethostbyname(host);
				if(server==NULL){
					//TODO: handle failed hostname lookups gracefully here
#ifdef DEBUG
//					fprintf(stderr,"Err: Could not find server\n");
#endif
					goto fail;
				}
				
				//configure the server address information
				bzero((char *)(&serv_addr),sizeof(serv_addr));
				serv_addr.sin_family=AF_INET;
				bcopy((char *)(server->h_addr),(char *)(&serv_addr.sin_addr.s_addr),server->h_length);
				serv_addr.sin_port=htons(port);
				//if we could successfully connect
				//(side effects happen during this call to connect())
				if(connect(new_socket_fd,(struct sockaddr *)(&serv_addr),sizeof(serv_addr))<0){
					//TODO: handle failed connections gracefully here
#ifdef DEBUG
//					fprintf(stderr,"Err: Could not connect to server\n");
#endif
					goto fail;
				}
				
				//clear out the server list because we're probably gonna be modifying it
				wclear(server_list);
				wmove(server_list,0,0);
				
				//set the socket non-blocking
				int flags=fcntl(new_socket_fd,F_GETFL,0);
				flags=(flags==-1)?0:flags;
				fcntl(new_socket_fd,F_SETFL,flags|O_NONBLOCK);
				
				//a flag to say if we've already added the sever
				char added=FALSE;
				
				//make some data structures for relevant information
				int server_index;
				for(server_index=0;server_index<MAX_SERVERS;server_index++){
					//if this is not already a valid server and we haven't put the new server anywhere
					//(as soon as we finish adding it we set added and effectively ignore it for the rest of this loop)
					if((servers[server_index]==NULL)&&(!added)){
						//make it one
						servers[server_index]=(irc_connection*)(malloc(sizeof(irc_connection)));
						//initialize the buffer to all NULL bytes
						int n;
						for(n=0;n<BUFFER_SIZE;n++){
							servers[server_index]->read_buffer[n]='\0';
						}
						servers[server_index]->socket_fd=new_socket_fd;
						
						//set the port information (in case we need to re-connect)
						servers[server_index]->port=port;
						
						//set the server name
						strncpy(servers[server_index]->server_name,host,BUFFER_SIZE);
						
						//set the current channel to be 0 (the system/debug channel)
						//a JOIN would add channels, but upon initial connection 0 is the only valid one
						servers[server_index]->current_channel=0;
						
						//set the default channel for various messages from the server that are not channel-specific
						//NOTE: this scheme should be able to be overloaded to treat PM conversations as channels
						servers[server_index]->channel_name[0]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
						strncpy(servers[server_index]->channel_name[0],"SERVER",BUFFER_SIZE);
						
						servers[server_index]->channel_topic[0]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
						strncpy(servers[server_index]->channel_topic[0],"(no topic for the server)",BUFFER_SIZE);
						
						//set the main chat window with scrollback
						//as we get lines worth storing we'll add them to this content, but for the moment it's blank
						servers[server_index]->channel_content[0]=(char**)(malloc(MAX_SCROLLBACK*sizeof(char*)));
						for(n=0;n<MAX_SCROLLBACK;n++){
							servers[server_index]->channel_content[0][n]=NULL;
						}
						
						//by default don't reconnect if connnection is lost
						servers[server_index]->reconnect=FALSE;
						
						//TODO: make keeping logs a setting, not just always true
						servers[server_index]->keep_logs=TRUE;
						if(servers[server_index]->keep_logs){
							//first make a directory for this server
							char file_location[BUFFER_SIZE];
							sprintf(file_location,"%s/.local/share/accirc/%s/%s",getenv("HOME"),LOGGING_DIRECTORY,servers[server_index]->server_name);
							if(verify_or_make_dir(file_location)){
								sprintf(file_location,"%s/.local/share/accirc/%s/%s/%s",getenv("HOME"),LOGGING_DIRECTORY,servers[server_index]->server_name,servers[server_index]->channel_name[0]);
								//note that if this call fails it will be set to NULL and hence be skipped over when writing logs
								servers[server_index]->log_file[0]=fopen(file_location,"a");
								if(servers[server_index]->log_file[0]==NULL){
									scrollback_output(server_index,0,"accirc: Err: could not make log file");
								}
							//this fails in a non-silent way, the user should know there was a problem
							//if we couldn't make the directory then don't keep logs rather than failing hard
							}else{
								scrollback_output(server_index,0,"accirc: Err: could not make logging directory");
								servers[server_index]->keep_logs=FALSE;
							}
						}
						
						//by default don't rejoin on kick
						servers[server_index]->rejoin_on_kick=FALSE;
						
						//by default the fallback nick is null
						strncpy(servers[server_index]->fallback_nick,"",BUFFER_SIZE);
						
						//there are no users in the SERVER channel
						servers[server_index]->user_names[0]=(char**)(malloc(MAX_NAMES*sizeof(char*)));
						for(n=0;n<MAX_NAMES;n++){
							servers[server_index]->user_names[0][n]=NULL;
						}
						
						//NULL out all other channels
						//note this starts from 1 since 0 is the SERVER channel
						for(n=1;n<MAX_CHANNELS;n++){
							servers[server_index]->channel_name[n]=NULL;
							servers[server_index]->channel_content[n]=NULL;
							servers[server_index]->channel_topic[n]=NULL;
							servers[server_index]->new_channel_content[n]=FALSE;
							servers[server_index]->log_file[n]=NULL;
							servers[server_index]->autojoin_channel[n]=NULL;
							servers[server_index]->user_names[n]=NULL;
						}
						
						//clear out the ident information (the user will provide it in a startup (.rc) file if they so desire)
						strncpy(servers[server_index]->ident,"",BUFFER_SIZE);
						
						//by default there is no new content on this server
						servers[server_index]->new_server_content=FALSE;
						
						//default the user's name to NULL until we get more information (NICK data)
						strncpy(servers[server_index]->nick,"",BUFFER_SIZE);
						
						//set the current server to be the one we just connected to
						current_server=server_index;
						
						//don't add this server again
						added=TRUE;
						
						//output the server information (since we set current_server to this, it'll get output anyway on the next loop)
					}else if(servers[server_index]!=NULL){
						//another server, output it to the server list
						wprintw(server_list,servers[server_index]->server_name);
						wprintw(server_list," | ");
					}
				}
				
				//refresh the server list
				wrefresh(server_list);
			}
		}else if(!strcmp(command,"exit")){
			done=TRUE;
			
			char quit_message[BUFFER_SIZE];
			
			if(!strcmp(parameters,"")){
				sprintf(quit_message,"QUIT :%s\n",DEFAULT_QUIT_MESSAGE);
			}else{
				sprintf(quit_message,"QUIT :%s\n",parameters);
			}
			
			int n;
			for(n=0;n<MAX_SERVERS;n++){
				if(servers[n]!=NULL){
					safe_send(servers[n]->socket_fd,quit_message);
				}
			}
		//move a server to the left
		}else if(!strcmp(command,"sl")){
			if(current_server>=0){
				//reset scrollback
				scrollback_end=-1;
				
				//pre-condition for the below loop, else we'd start where we are
				if(current_server>0){
					current_server--;
				//at the end loop back around
				}else if(current_server==0){
					current_server=MAX_SERVERS-1;
				}
				
				int index;
				for(index=current_server;index>=0;index--){
					if(servers[index]!=NULL){
						current_server=index;
						index=-1;
					//if current index is negative there never was a server so just die and don't change it
					}else if((index==0)&&(current_server>=0)){
						//go back to the start, at worst we'll end up where we were
						//note this is MAX_SERVERS because it gets decremented before the next loop iteration
						index=MAX_SERVERS;
					}
				}
			}
		//move a server to the right
		}else if(!strcmp(command,"sr")){
			if(current_server>=0){
				//reset scrollback
				scrollback_end=-1;
				
				//pre-condition for the below loop, else we'd start where we are
				if(current_server<(MAX_SERVERS-1)){
					current_server++;
				//at the end loop back around
				}else if(current_server==MAX_SERVERS-1){
					current_server=0;
				}
				
				int index;
				for(index=current_server;index<MAX_SERVERS;index++){
					if(servers[index]!=NULL){
						current_server=index;
						index=MAX_SERVERS;
					//if current index is negative there never was a server so just die and don't change it
					}else if((index==(MAX_SERVERS-1))&&(current_server>=0)){
						//go back to the start, at worst we'll end up where we were
						//note this is -1 because it gets incremented before the next loop iteration
						index=-1;
					}
				}
			}
		//move a channel to the left
		}else if(!strcmp(command,"cl")){
			if(current_server>=0){
				//reset scrollback
				scrollback_end=-1;
				
				int current_channel=servers[current_server]->current_channel;
				//pre-condition for the below loop, else we'd start where we are
				if(current_channel>0){
					current_channel--;
				//at the end loop back around
				}else if(current_channel==0){
					current_channel=MAX_CHANNELS-1;
				}
				
				int index;
				for(index=current_channel;index>=0;index--){
					if(servers[current_server]->channel_name[index]!=NULL){
						current_channel=index;
						index=-1;
					}else if(index==0){
						//go back to the start, at worst we'll end up where we were
						//note this is MAX_CHANNELS because it gets decremented before the next loop iteration
						index=MAX_CHANNELS;
					}
				}
				
				servers[current_server]->current_channel=current_channel;
			}
		//move a channel to the right
		}else if(!strcmp(command,"cr")){
			if(current_server>=0){
				//reset scrollback
				scrollback_end=-1;
				
				int current_channel=servers[current_server]->current_channel;
				//pre-condition for the below loop, else we'd start where we are
				if(current_channel<(MAX_CHANNELS-1)){
					current_channel++;
				//at the end loop back around
				}else if(current_channel==(MAX_CHANNELS-1)){
					current_channel=0;
				}
				
				int index;
				for(index=current_channel;index<MAX_CHANNELS;index++){
					if(servers[current_server]->channel_name[index]!=NULL){
						current_channel=index;
						index=MAX_CHANNELS;
					}else if(index==(MAX_CHANNELS-1)){
						//go back to the start, at worst we'll end up where we were
						//note this is -1 because it gets incremented before the next loop iteration
						index=-1;
					}
				}
				
				servers[current_server]->current_channel=current_channel;
			}
		//CTCP ACTION, bound to the common "/me"
		}else if(!strcmp(command,"me")){
			if(current_server>=0){
				//attached the control data and recurse
				char tmp_buffer[BUFFER_SIZE];
				sprintf(tmp_buffer,"%cACTION %s%c",0x01,parameters,0x01);
				//don't keep that in the history though
				parse_input(tmp_buffer,FALSE);
			}
		//sleep command
		}else if(!strcmp(command,"sleep")){
			scrollback_output(current_server,0,"accirc: sleeping...");
			//sleep as requested (in seconds)
			sleep(atoi(parameters));
		//usleep command
		}else if(!strcmp(command,"usleep")){
			scrollback_output(current_server,0,"accirc: usleeping...");
			//sleep as requested (in milliseconds)
			usleep(atoi(parameters));
		//comment command (primarily for the .rc file)
		}else if(!strcmp(command,"comment")){
			//ignore it
		//automatically join this channel when possible (after the server sends us a message saying we're connected)
		}else if(!strcmp(command,"autojoin")){
			if(current_server>=0){
				int ch;
				for(ch=0;(ch<MAX_CHANNELS)&&(servers[current_server]->autojoin_channel[ch]!=NULL);ch++);
				if(ch<MAX_CHANNELS){
					servers[current_server]->autojoin_channel[ch]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
					strncpy(servers[current_server]->autojoin_channel[ch],parameters,BUFFER_SIZE);
					scrollback_output(current_server,0,"accirc: autojoin channel added, will join when possible");
				}else{
					scrollback_output(current_server,0,"accirc: autojoin channel could not be added, would overflow");
				}
			}
		//automatically identify when the server is ready to recieve that information
		}else if(!strcmp(command,"autoident")){
			if(current_server>=0){
				strncpy(servers[current_server]->ident,parameters,BUFFER_SIZE);
				scrollback_output(current_server,0,"accirc: autoident given, will ident when possible");
			}
		//a nick to fall back to if the nick you want is taken
		}else if(!strcmp(command,"fallback_nick")){
			if(current_server>=0){
				strncpy(servers[current_server]->fallback_nick,parameters,BUFFER_SIZE);
				scrollback_output(current_server,0,"accirc: fallback_nick set");
			}
		}else if(!strcmp(command,"rejoin_on_kick")){
			if(current_server>=0){
				servers[current_server]->rejoin_on_kick=TRUE;
				scrollback_output(current_server,0,"accirc: rejoin_on_kick set to TRUE");
			}
		}else if(!strcmp(command,"no_rejoin_on_kick")){
			if(current_server>=0){
				servers[current_server]->rejoin_on_kick=FALSE;
				scrollback_output(current_server,0,"accirc: rejoin_on_kick set to FALSE");
			}
		}else if(!strcmp(command,"reconnect")){
			if(current_server>=0){
				servers[current_server]->reconnect=TRUE;
				scrollback_output(current_server,0,"accirc: reconnect set to TRUE");
			}
		}else if(!strcmp(command,"no_reconnect")){
			if(current_server>=0){
				servers[current_server]->reconnect=FALSE;
				scrollback_output(current_server,0,"accirc: reconnect set to FALSE");
			}
		//unknown command error
		}else{
			if(current_server>=0){
				char error_buffer[BUFFER_SIZE];
				sprintf(error_buffer,"accirc: Err: unknown command \"%s\"",command);
				scrollback_output(current_server,0,error_buffer);
			}
		}
	//if it's a server command send the raw text to the server
	}else if(server_command){
		//if we're connected to a server
		if(current_server>=0){
			char to_send[BUFFER_SIZE];
			sprintf(to_send,"%s\n",input_buffer);
			safe_send(servers[current_server]->socket_fd,to_send);
			
			//format the text for my viewing benefit (this is also what will go in logs, with a newline)
			char output_buffer[BUFFER_SIZE];
	//		sprintf(output_buffer,"%lu %s",(uintmax_t)(time(NULL)),input_buffer);
			sprintf(output_buffer,"%s",input_buffer);
			
			//place my own text in the scrollback for this server and channel
			scrollback_output(current_server,0,output_buffer);
			
			//refresh the channel text just in case
			refresh_channel_text();
		}
	//if it's not a command of any kind send it as a PM to current channel and server
	}else{
		if(current_server>=0){
			//if we're on the server channel treat it as a command (recurse)
			if(servers[current_server]->current_channel==0){
				char tmp_buffer[BUFFER_SIZE];
				sprintf(tmp_buffer,":%s",input_buffer);
				//but don't keep history for this recursion call
				parse_input(tmp_buffer,FALSE);
			}else{
				//format the text for the server's benefit
				char output_buffer[BUFFER_SIZE];
				sprintf(output_buffer,"PRIVMSG %s :%s\n",servers[current_server]->channel_name[servers[current_server]->current_channel],input_buffer);
				safe_send(servers[current_server]->socket_fd,output_buffer);
				
				//then format the text for my viewing benefit (this is also what will go in logs, with a newline)
				//accounting specially for if the user sent a CTCP ACTION
				char ctcp[BUFFER_SIZE];
				sprintf(ctcp,"%cACTION ",0x01);
				if(strfind(ctcp,input_buffer)==0){
					char tmp_buffer[BUFFER_SIZE];
					substr(tmp_buffer,input_buffer,strlen(ctcp),strlen(input_buffer)-strlen(ctcp)-1);
//					sprintf(output_buffer,">> %lu *%s %s",(uintmax_t)(time(NULL)),servers[current_server]->nick,tmp_buffer);
					sprintf(output_buffer,">> *%s %s",servers[current_server]->nick,tmp_buffer);
				}else{
//					sprintf(output_buffer,">> %lu <%s> %s",(uintmax_t)(time(NULL)),servers[current_server]->nick,input_buffer);
					sprintf(output_buffer,">> <%s> %s",servers[current_server]->nick,input_buffer);
				}
				
				//place my own text in the scrollback for this server and channel
				scrollback_output(current_server,servers[current_server]->current_channel,output_buffer);
			}
		}else{
#ifdef DEBUG
//			int foreground,background;
//			sscanf(input_buffer,"%i %i",&foreground,&background);
//			wclear(channel_text);
//			wcoloron(channel_text,foreground,background);
//			wprintw(channel_text,"This is a sample string in fg=%i bg=%i",foreground,background);
//			wcoloroff(channel_text,foreground,background);
//			wrefresh(channel_text);
#endif
		}
	}
fail:	
	strncpy(input_buffer,"\0",BUFFER_SIZE);
}

void parse_server(int server_index){
	//clear out the remainder of the buffer since we re-use this memory
	int n;
	for(n=strlen(servers[server_index]->read_buffer);n<BUFFER_SIZE;n++){
		servers[server_index]->read_buffer[n]='\0';
	}
	
	//parse in whatever the server sent and display it appropriately
	int first_delimeter=strfind(" :",servers[server_index]->read_buffer);
	char command[BUFFER_SIZE];
	if(first_delimeter>0){
		substr(command,servers[server_index]->read_buffer,0,first_delimeter);
	}else{
		strncpy(command,"",BUFFER_SIZE);
	}
	
	if(!strcmp(command,"PING")){
		//TODO: make this less hacky than it is
		//switch the I in ping for an O in pong
		servers[server_index]->read_buffer[1]='O';
		safe_send(servers[server_index]->socket_fd,servers[server_index]->read_buffer);
	//if we got an error, close the link and clean up the structures
	}else if(!strcmp(command,"ERROR")){
		properly_close(server_index);
	}else{
		//set this to show as having new data, it must since we're getting something on it
		servers[server_index]->new_server_content=TRUE;
		refresh_server_list();
		
		//take out the trailing newline (accounting for the possibility of windows newlines
		int newline_index=strfind("\r\n",servers[server_index]->read_buffer);
		if(newline_index<0){
			newline_index=strfind("\n",servers[server_index]->read_buffer);
		}
		//NOTE: I can set this to be a substring of itself since I'm not overwriting anything during copy that I'll need
		substr(servers[server_index]->read_buffer,servers[server_index]->read_buffer,0,newline_index);
		
		//a flag to say if we already output within the handling
		//(if so don't output again at the end)
		char special_output=FALSE;
		
		//what gets shown in logs and user scrollback, which may differ from the raw server data
		char output_buffer[BUFFER_SIZE];
		strncpy(output_buffer,servers[server_index]->read_buffer,BUFFER_SIZE);
		
		//the channel to output to, by default the SYSTEM channel
		int output_channel=0;
		
		//parse PMs and such and don't just ALWAYS go to the system channel
		int first_space=strfind(" ",servers[server_index]->read_buffer);
		if(first_space>=0){
			//the start at 1 is to cut off the preceding ":"
			//remember the second arguement to substr is a LENGTH, not an index
			substr(command,servers[server_index]->read_buffer,1,first_space-1);
			
			//TODO: make this less hacky, it works but... well, hacky
			//NOTE:checking for the literal server name was giving me issues because sometimes a server will re-direct to another one, so this just checks in general "is any valid server name?"
//			//if this message started with the server's name
//			if(!strcmp(command,servers[server_index]->server_name)){
			//check that it is NOT a user (meaning it must not have the delimeter chars for a username)
			if(strfind("@",command)==-1){
				//these messages have the form ":naos.foonetic.net 001 accirc_user :Welcome to the Foonetic IRC Network nick!realname@hostname.could.be.ipv6"
				substr(command,servers[server_index]->read_buffer,1,strlen(servers[server_index]->read_buffer)-1);
				
				first_space=strfind(" ",command);
				char tmp_buffer[BUFFER_SIZE];
				substr(tmp_buffer,command,first_space+1,strlen(command)-first_space-1);
				substr(command,tmp_buffer,0,strfind(" ",tmp_buffer));
				
				//welcome message (we set the server NICK data here since it's clearly working for us)
				if(!strcmp(command,"001")){
					//rather than make a new buffer just use the one it'll ulitmately be stored in
					char *user_nick=servers[server_index]->nick;
					
					//go a space at a time until we get to the nick portion, then leave because it's set
					first_space=strfind(" ",servers[server_index]->read_buffer);
					substr(user_nick,servers[server_index]->read_buffer,first_space+1,strlen(servers[server_index]->read_buffer)-first_space);
					first_space=strfind(" ",user_nick);
					substr(tmp_buffer,user_nick,first_space+1,strlen(user_nick)-first_space);
					substr(user_nick,tmp_buffer,0,strfind(" ",tmp_buffer));
					
					//let the user know his nick is recognized
					refresh_server_list();
					
					//if we got here the server recognized us, go through the autojoin channels and join them, then remove them from the autojoin array
					int ch;
					for(ch=0;ch<MAX_CHANNELS;ch++){
						if(servers[server_index]->autojoin_channel[ch]!=NULL){
							//remember what server we were on
							int old_server=current_server;
							
							//parse a message headed to the server to autojoin on
							current_server=server_index;
							
							char to_parse[BUFFER_SIZE];
							sprintf(to_parse,":join :%s",servers[server_index]->autojoin_channel[ch]);
							parse_input(to_parse,FALSE);
							
							//remove this from autojoin channels from that server, we've now at least attempted to join
							free(servers[server_index]->autojoin_channel[ch]);
							servers[server_index]->autojoin_channel[ch]=NULL;
							
							//reset our server to whatever it used to be on
							current_server=old_server;
						}
					}
					
					//if there is associated identification with this nickname send it to nickserv now
					if(strcmp(servers[server_index]->ident,"")!=0){
						//remember what server we were on
						int old_server=current_server;
						
						//parse a message headed to the server to autojoin on
						current_server=server_index;
						
						char to_parse[BUFFER_SIZE];
						sprintf(to_parse,":privmsg NickServ :IDENTIFY %s",servers[server_index]->ident);
						parse_input(to_parse,FALSE);
						
						//reset our server to whatever it used to be on
						current_server=old_server;
					}
				//current channel topic
				}else if(!strcmp(command,"332")){
					char channel[BUFFER_SIZE];
					char topic[BUFFER_SIZE];
					
					//pull out the topic right away, because it's delimted by " :" and so is easy
					int space_colon_index=strfind(" :",servers[server_index]->read_buffer);
					substr(topic,servers[server_index]->read_buffer,space_colon_index+2,strlen(servers[server_index]->read_buffer)-space_colon_index-2);
					
					strncpy(tmp_buffer,servers[server_index]->read_buffer,BUFFER_SIZE);
					
					//go a space at a time until we get to the relevant field
					int n;
					for(n=0;n<3;n++){
						//note I can set tmp_buffer to a substring of itself here because I'm never overwriting data I'll later need
						//it's just a left shift
						first_space=strfind(" ",tmp_buffer);
						substr(tmp_buffer,tmp_buffer,first_space+1,strlen(tmp_buffer)-first_space-1);
					}
					
					//now we're at the channel, get it and lower-case it
					substr(channel,tmp_buffer,0,strfind(" ",tmp_buffer));
					strtolower(channel,BUFFER_SIZE);
					
					//go through the channels, find out the one to output to, set "output_channel" to that index
					//note that if we never find the channel output_channel stays at its default, which is the SERVER channel
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							char lower_case_channel[BUFFER_SIZE];
							strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
							strtolower(lower_case_channel,BUFFER_SIZE);
							
							if(!strcmp(channel,lower_case_channel)){
								output_channel=channel_index;
								
								sprintf(output_buffer,"TOPIC for %s :%s",channel,topic);
								
								//store the topic in the general data structure
								strncpy(servers[server_index]->channel_topic[channel_index],topic,BUFFER_SIZE);
								
								//and output
								refresh_channel_topic();
								
								channel_index=MAX_CHANNELS;
							}
						}
					}
				//handle time set information for a channel topic
				}else if(!strcmp(command,"333")){
					char channel[BUFFER_SIZE];
					
					//go a space at a time until we get to the relevant field
					int n;
					for(n=0;n<2;n++){
						//note I can set tmp_buffer to a substring of itself here because I'm never overwriting data I'll later need
						//it's just a left shift
						first_space=strfind(" ",tmp_buffer);
						substr(tmp_buffer,tmp_buffer,first_space+1,strlen(tmp_buffer)-first_space-1);
					}
					
					//now we're at the channel, get it and lower-case it
					first_space=strfind(" ",tmp_buffer);
					substr(channel,tmp_buffer,0,first_space);
					strtolower(channel,BUFFER_SIZE);
					
					substr(tmp_buffer,tmp_buffer,first_space+1,strlen(tmp_buffer)-first_space-1);
					
					//now we're at the user who set this topic
					first_space=strfind(" ",tmp_buffer);
					char setting_user[BUFFER_SIZE];
					substr(setting_user,tmp_buffer,0,first_space);
					
					substr(tmp_buffer,tmp_buffer,first_space+1,strlen(tmp_buffer)-first_space-1);
					
					//now we're at the timestamp
					time_t timestamp=atoi(tmp_buffer);
					
					//go through the channels, find out the one to output to, set "output_channel" to that index
					//note that if we never find the channel output_channel stays at its default, which is the SERVER channel
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							char lower_case_channel[BUFFER_SIZE];
							strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
							strtolower(lower_case_channel,BUFFER_SIZE);
							
							if(!strcmp(channel,lower_case_channel)){
								output_channel=channel_index;
								
								sprintf(output_buffer,"Topic set by %s at %s",setting_user,ctime(&timestamp));
								
								//and output
								refresh_channel_topic();
								
								channel_index=MAX_CHANNELS;
							}
						}
					}
				//names list
				//(like this: ":naos.foonetic.net 353 accirc_user @ #FaiD3.0 :accirc_user @neutrak @NieXS @cheese @MonkeyofDoom @L @Data @Spock ~Shishichi davean")
				//(or this: ":naos.foonetic.net 353 neutrak = #FaiD :neutrak mo0 Decarabia Gelsamel_ NieXS JoeyJo0 cheese")
				}else if(!strcmp(command,"353")){
					strncpy(tmp_buffer,servers[server_index]->read_buffer,BUFFER_SIZE);
					char channel[BUFFER_SIZE];
					int space_colon_index=strfind(" :",tmp_buffer);
					int channel_start_index=space_colon_index-1;
					while(tmp_buffer[channel_start_index]!=' '){
						channel_start_index--;
					}
					channel_start_index++;
					
					substr(channel,tmp_buffer,channel_start_index,space_colon_index-channel_start_index);
					//lower case the channel name
					strtolower(channel,BUFFER_SIZE);
					
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							char lower_case_channel[BUFFER_SIZE];
							strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
							strtolower(lower_case_channel,BUFFER_SIZE);
							
							if(!strcmp(channel,lower_case_channel)){
								output_channel=channel_index;
								channel_index=MAX_CHANNELS;
							}
						}
					}
					
					//if we found this channel in our list
					if(output_channel>0){
						char names[BUFFER_SIZE];
						substr(names,tmp_buffer,space_colon_index+2,strlen(tmp_buffer)-space_colon_index-2);
						while(strfind(" ",names)>=0){
							char this_name[BUFFER_SIZE];
							int space_index=strfind(" ",names);
							substr(this_name,names,0,space_index);
							substr(names,names,space_index+1,strlen(names)-space_index-1);
							
							//trim this user's name
							if((strfind("@",this_name)==0)||(strfind("~",this_name)==0)||(strfind("%",this_name)==0)||(strfind("&",this_name)==0)||(strfind("+",this_name)==0)){
								substr(this_name,this_name,1,strlen(this_name)-1);
							}
							
							//check if this user is already in the list for this channel
							int matches=0;
							
							char this_lower_case_name[BUFFER_SIZE];
							strncpy(this_lower_case_name,this_name,BUFFER_SIZE);
							strtolower(this_lower_case_name,BUFFER_SIZE);
							
							int n;
							for(n=0;n<MAX_NAMES;n++){
								if(servers[server_index]->user_names[output_channel][n]!=NULL){
									char matching_name[BUFFER_SIZE];
									strncpy(matching_name,servers[server_index]->user_names[output_channel][n],BUFFER_SIZE);
									strtolower(matching_name,BUFFER_SIZE);
									
									//found this nick
									if(!strcmp(this_lower_case_name,matching_name)){
										matches++;
										//if it was a duplicate remove this copy
										if(matches>1){
											free(servers[server_index]->user_names[output_channel][n]);
											servers[server_index]->user_names[output_channel][n]=NULL;
											matches--;
										}
									}
								}
							}
							
							//if the user wasn't already there
							if(matches==0){
								//find a spot for a new user
								for(n=0;((servers[server_index]->user_names[output_channel][n]!=NULL)&&(n<MAX_NAMES));n++);
								if(n<MAX_NAMES){
									servers[server_index]->user_names[output_channel][n]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
									strncpy(servers[server_index]->user_names[output_channel][n],this_name,BUFFER_SIZE);
								}
							}
						}
#ifdef DEBUG
//						int n;
//						for(n=0;n<MAX_NAMES;n++){
//							if(servers[server_index]->user_names[output_channel][n]!=NULL){
//								scrollback_output(server_index,0,servers[server_index]->user_names[output_channel][n]);
//							}
//						}
#endif
					}
				//end of names list
				}else if(!strcmp(command,"366")){
					//just output to the right channel
					strncpy(tmp_buffer,servers[server_index]->read_buffer,BUFFER_SIZE);
					char channel[BUFFER_SIZE];
					int space_colon_index=strfind(" :",tmp_buffer);
					int channel_start_index=space_colon_index-1;
					while(tmp_buffer[channel_start_index]!=' '){
						channel_start_index--;
					}
					channel_start_index++;
					
					substr(channel,tmp_buffer,channel_start_index,space_colon_index-channel_start_index);
					//lower case the channel name
					strtolower(channel,BUFFER_SIZE);
					
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							char lower_case_channel[BUFFER_SIZE];
							strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
							strtolower(lower_case_channel,BUFFER_SIZE);
							
							if(!strcmp(channel,lower_case_channel)){
								output_channel=channel_index;
								channel_index=MAX_CHANNELS;
							}
						}
					}
				//end of message of the day (useful as a delimeter)
				}else if(!strcmp(command,"376")){
					
				//nick already in use, so try a new one
				}else if(!strcmp(command,"433")){
					char new_nick[BUFFER_SIZE];
					sprintf(new_nick,"%s_",servers[server_index]->fallback_nick);
					//in case this fails again start with another _ for the next try
					strncpy(servers[server_index]->fallback_nick,new_nick,BUFFER_SIZE);
					
					sprintf(new_nick,"NICK %s\n",servers[server_index]->fallback_nick);
					safe_send(servers[server_index]->socket_fd,new_nick);
				}
			//a message from another user
			}else{
				//a temporary buffer to store intermediate results during parsing
				char tmp_buffer[BUFFER_SIZE];
				
				//declarations for various things worth parsing out
				char nick[BUFFER_SIZE];
				char real_name[BUFFER_SIZE];
				char hostmask[BUFFER_SIZE];
				char command[BUFFER_SIZE];
				char text[BUFFER_SIZE];
				
				//user's nickname is delimeted by "!"
				int exclam_index=strfind("!",servers[server_index]->read_buffer);
				//start at 1 to cut off the leading ":"
				substr(nick,servers[server_index]->read_buffer,1,exclam_index-1);
				
				//move past that point so we have a tmp_buffer after it (we'll be doing this a lot)
				substr(tmp_buffer,servers[server_index]->read_buffer,exclam_index+1,strlen(servers[server_index]->read_buffer)-exclam_index-1);
				
				//user's real name is delimeted by "@"
				int at_index=strfind("@",tmp_buffer);
				substr(real_name,tmp_buffer,0,at_index);
				
				//I CAN set tmp_buffer to a substring of itself here BECAUSE it'll just shift everything left and never overwrite what it needs to use
				substr(tmp_buffer,tmp_buffer,at_index+1,strlen(tmp_buffer)-at_index-1);
				
				//hostmask is delimeted by " "
				int space_index=strfind(" ",tmp_buffer);
				substr(hostmask,tmp_buffer,0,space_index);
				
				substr(tmp_buffer,tmp_buffer,space_index+1,strlen(tmp_buffer)-space_index-1);
				
				//command is delimeted by " "
				space_index=strfind(" ",tmp_buffer);
				substr(command,tmp_buffer,0,space_index);
				
				substr(tmp_buffer,tmp_buffer,space_index+1,strlen(tmp_buffer)-space_index-1);
				
				//the rest of the string is the text
				strncpy(text,tmp_buffer,BUFFER_SIZE);
				
				//start of command handling
				//the most common message, the PM
				//":neutrak!neutrak@hide-F99E0499.device.mst.edu PRIVMSG accirc_user :test"
				if(!strcmp(command,"PRIVMSG")){
					char channel[BUFFER_SIZE];
					int space_colon_index=strfind(" :",tmp_buffer);
					substr(channel,tmp_buffer,0,space_colon_index);
					
					substr(tmp_buffer,tmp_buffer,space_colon_index+2,strlen(tmp_buffer)-space_colon_index-2);
					
					strncpy(text,tmp_buffer,BUFFER_SIZE);
					
					//lower case the channel so we can do a case-insensitive string match against it
					strtolower(channel,BUFFER_SIZE);
					
					//go through the channels, find out the one to output to, set "output_channel" to that index
					//note that if we never find the channel output_channel stays at its default, which is the SERVER channel
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							char lower_case_channel[BUFFER_SIZE];
							strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
							strtolower(lower_case_channel,BUFFER_SIZE);
							
							if(!strcmp(channel,lower_case_channel)){
								output_channel=channel_index;
								channel_index=MAX_CHANNELS;
							}
						}
					}
					
					char tmp_nick[BUFFER_SIZE];
					strncpy(tmp_nick,servers[server_index]->nick,BUFFER_SIZE);
					strtolower(tmp_nick,BUFFER_SIZE);
					
					if(!strcmp(tmp_nick,channel)){
#ifdef DEBUG
						char sys_call_buffer[BUFFER_SIZE];
						sprintf(sys_call_buffer,"echo \"%lu <%s> %s\" | mail -s \"PM\" \"%s\"",(uintmax_t)(time(NULL)),nick,text,servers[server_index]->nick);
						system(sys_call_buffer);
#endif
					}
					
					//for pings
					int name_index=strfind(servers[server_index]->nick,text);
					
					//for any CTCP message (which takes highest precedence)
					char ctcp[BUFFER_SIZE];
					sprintf(ctcp,"%c",0x01);
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
							sprintf(ctcp,"%c",0x01);
							if(strfind(ctcp,tmp_buffer)>=0){
								tmp_buffer[strfind(ctcp,tmp_buffer)]='\0';
							}
							strncpy(ctcp,tmp_buffer,BUFFER_SIZE);
						}
						
						//be case-insensitive
						strtolower(ctcp,BUFFER_SIZE);
						
						//handle CTCP ACTION
						if(!strcmp(ctcp,"action")){
							int offset=strlen("action");
							char tmp_buffer[BUFFER_SIZE];
							//the +1 and -1 is because we want to start AFTER the ctcp command, and ctcp_check is AT that byte
							//and another +1 and -1 because we don't want to include the space the delimits the CTCP command from the rest of the message
							substr(tmp_buffer,text,ctcp_check+offset+2,strlen(text)-ctcp_check-offset-2);
							
							//this accounts for a possible trailing byte
							sprintf(ctcp,"%c",0x01);
							if(strfind(ctcp,tmp_buffer)>=0){
								tmp_buffer[strfind(ctcp,tmp_buffer)]='\0';
							}
							
							//note: timestamps are added at the end
							sprintf(output_buffer,"*%s %s",nick,tmp_buffer);
						//handle CTCP VERSION
						}else if(!strcmp(ctcp,"version")){
							int offset=strlen("version");
							char tmp_buffer[BUFFER_SIZE];
							//the +1 and -1 is because we want to start AFTER the ctcp command, and ctcp_check is AT that byte
							//and another +1 and -1 because we don't want to include the space the delimits the CTCP command from the rest of the message
							substr(tmp_buffer,text,ctcp_check+offset+2,strlen(text)-ctcp_check-offset-2);
							
							//some clients prefer privmsg responses, others prefer notice response
							int old_server=current_server;
							current_server=server_index;
//							sprintf(ctcp,":privmsg %s :%cVERSION accidental_irc v%s%c",nick,0x01,VERSION,0x01);
//							parse_input(ctcp,FALSE);
							sprintf(ctcp,":notice %s :%cVERSION accidental_irc v%s%c",nick,0x01,VERSION,0x01);
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
							sprintf(ctcp,":notice %s :%s",nick,text);
							parse_input(ctcp,FALSE);
							current_server=old_server;
							
							refresh_server_list();
							refresh_channel_list();
							refresh_channel_topic();
							refresh_channel_text();
						}
					//handle for a ping (when someone says our own nick)
					}else if(name_index>=0){
#ifdef DEBUG
						//take any desired additional steps upon ping here (could add notify-send or something, if desired)
						char sys_call_buffer[BUFFER_SIZE];
						sprintf(sys_call_buffer,"echo \"%lu ***<%s> %s\" | mail -s \"PING\" \"%s\"",(uintmax_t)(time(NULL)),nick,text,servers[server_index]->nick);
						system(sys_call_buffer);
#endif
						//audio output
						beep();
						//format the output to show that we were pingged
						sprintf(output_buffer,"***<%s> %s",nick,text);
					}else{
						//format the output of a PM in a very pretty way
						sprintf(output_buffer,"<%s> %s",nick,text);
					}
				//":accirc_2!1@hide-68F46812.device.mst.edu JOIN :#FaiD3.0"
				}else if(!strcmp(command,"JOIN")){
					//if it was us doing the join-ing
					if(!strcmp(servers[server_index]->nick,nick)){
						//TODO: make sure we're not already in this channel
						//add this channel to the list of channels on this server, make associated scrollback, etc.
						int channel_index;
						for(channel_index=0;(channel_index<MAX_CHANNELS)&&(servers[server_index]->channel_name[channel_index]!=NULL);channel_index++);
						if(channel_index<MAX_CHANNELS){
							servers[server_index]->channel_name[channel_index]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
							//initialize the channel name to be what was joined
							//leaving out the leading ":"
							substr(servers[server_index]->channel_name[channel_index],text,1,strlen(text)-1);
							
							//default to a null topic
							servers[server_index]->channel_topic[channel_index]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
							strncpy(servers[server_index]->channel_topic[channel_index],"",BUFFER_SIZE);
							
							servers[server_index]->channel_content[channel_index]=(char**)(malloc(MAX_SCROLLBACK*sizeof(char*)));
							//null out the content to start with
							int n;
							for(n=0;n<MAX_SCROLLBACK;n++){
								servers[server_index]->channel_content[channel_index][n]=NULL;
							}
							
							servers[server_index]->user_names[channel_index]=(char**)(malloc(MAX_NAMES*sizeof(char*)));
							for(n=0;n<MAX_NAMES;n++){
								servers[server_index]->user_names[channel_index][n]=NULL;
							}
							
							//if we should be keeping logs make sure we are
							if(servers[server_index]->keep_logs){
								char file_location[BUFFER_SIZE];
								sprintf(file_location,"%s/.local/share/accirc/%s/%s/%s",getenv("HOME"),LOGGING_DIRECTORY,servers[server_index]->server_name,servers[server_index]->channel_name[channel_index]);
								//note if this fails it will be set to NULL and hence will be skipped over when trying to output to it
								servers[server_index]->log_file[channel_index]=fopen(file_location,"a");
								
								
								if(servers[server_index]->log_file[channel_index]!=NULL){
									//turn off buffering since I need may this output immediately and buffers annoy me for that
									setvbuf(servers[server_index]->log_file[channel_index],NULL,_IONBF,0);
								}
							}
							
							//set this to be the current channel, we must want to be here if we joined
							servers[server_index]->current_channel=channel_index;
							
							//output the join at the top of this channel, why not
							output_channel=channel_index;
							
							//and refresh the channel list
							refresh_channel_list();
							
						//TODO: handle being out of available channels more gracefully
						//at the moment this will just not have the new channel available, and as a result redirect all output to the system channel
						//which is not terrible I guess but not ideal
						}
					//else it wasn't us doing the join so just output the join message to that channel (which presumably we're in)
					}else{
						char channel[BUFFER_SIZE];
						//cut the leading : from the channel name
						substr(channel,text,1,strlen(text)-1);
						
						//lower case the channel so we can do a case-insensitive string match against it
						strtolower(channel,BUFFER_SIZE);
						
						int channel_index;
						for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
							if(servers[server_index]->channel_name[channel_index]!=NULL){
								char lower_case_channel[BUFFER_SIZE];
								strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
								strtolower(lower_case_channel,BUFFER_SIZE);
								
								if(!strcmp(lower_case_channel,channel)){
									//add this user to that channel's names array
									int n;
									for(n=0;(servers[server_index]->user_names[channel_index][n]!=NULL)&&(n<MAX_NAMES);n++);
									if(n<MAX_NAMES){
										servers[server_index]->user_names[channel_index][n]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
										strncpy(servers[server_index]->user_names[channel_index][n],nick,BUFFER_SIZE);
									}
									
									output_channel=channel_index;
									channel_index=MAX_CHANNELS;
								}
							}
						}
					}
				//or ":neutrak_accirc!1@sirc-8B6227B6.device.mst.edu PART #randomz"
				}else if(!strcmp(command,"PART")){
					char channel[BUFFER_SIZE];
					
					int space_colon_index=strfind(" :",text);
					if(space_colon_index<0){
						strncpy(channel,text,BUFFER_SIZE);
					}else{
						substr(channel,text,0,space_colon_index);
					}
					
					//if it was us doing the part
					if(!strcmp(servers[server_index]->nick,nick)){
						leave_channel(server_index,channel);
						output_channel=0;
					//else it wasn't us doing the part so just output the part message to that channel (which presumably we're in)
					}else{
						strtolower(channel,BUFFER_SIZE);
						
						int channel_index;
						for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
							if(servers[server_index]->channel_name[channel_index]!=NULL){
								char lower_case_channel[BUFFER_SIZE];
								strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
								strtolower(lower_case_channel,BUFFER_SIZE);
								
								if(!strcmp(lower_case_channel,channel)){
									output_channel=channel_index;
									channel_index=MAX_CHANNELS;
								}
							}
						}
						
						//set the user's nick to be lower-case for case-insensitive string matching
						strtolower(nick,BUFFER_SIZE);
						
						//remove this user from the names array of the channel she/he parted
						channel_index=output_channel;
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							int name_index;
							for(name_index=0;name_index<MAX_NAMES;name_index++){
								if(servers[server_index]->user_names[channel_index][name_index]!=NULL){
									char this_name[BUFFER_SIZE];
									strncpy(servers[server_index]->user_names[channel_index][name_index],this_name,BUFFER_SIZE);
									strtolower(this_name,BUFFER_SIZE);
									if(!strcmp(this_name,nick)){
										//remove this user from that channel's names array
										free(servers[server_index]->user_names[channel_index][name_index]);
										servers[server_index]->user_names[channel_index][name_index]=NULL;
									}
								}
							}
						}
						//note special_output is still false here, we never output up there
					}
				//or ":Shishichi!notIRCuser@hide-4C94998D.fidnet.com KICK #FaiD3.0 accirc_user :accirc_user: I need a kick message real quick"
				}else if(!strcmp(command,"KICK")){
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
					if(!strcmp(kicked_user,servers[server_index]->nick)){
						leave_channel(server_index,channel);
						output_channel=0;
						
						//if we're to rejoin on a kick do that now
						if(servers[server_index]->rejoin_on_kick){
							int old_server=current_server;
							current_server=server_index;
							char to_parse[BUFFER_SIZE];
							sprintf(to_parse,":join %s",channel);
							parse_input(to_parse,FALSE);
							current_server=old_server;
							
							refresh_server_list();
							refresh_channel_list();
							refresh_channel_text();
						}
					//else it wasn't us getting kicked so just output the join message to that channel (which presumably we're in)
					}else{
						strtolower(channel,BUFFER_SIZE);
						
						int channel_index;
						for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
							if(servers[server_index]->channel_name[channel_index]!=NULL){
								char lower_case_channel[BUFFER_SIZE];
								strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
								strtolower(lower_case_channel,BUFFER_SIZE);
								
								if(!strcmp(lower_case_channel,channel)){
									output_channel=channel_index;
									channel_index=MAX_CHANNELS;
								}
							}
						}
						
						//set the user's nick to be lower-case for case-insensitive string matching
						strtolower(nick,BUFFER_SIZE);
						
						//remove this user from the names array of the channel she/he was kicked out of
						channel_index=output_channel;
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							int name_index;
							for(name_index=0;name_index<MAX_NAMES;name_index++){
								if(servers[server_index]->user_names[channel_index][name_index]!=NULL){
									char this_name[BUFFER_SIZE];
									strncpy(servers[server_index]->user_names[channel_index][name_index],this_name,BUFFER_SIZE);
									strtolower(this_name,BUFFER_SIZE);
									if(!strcmp(this_name,nick)){
										//remove this user from that channel's names array
										free(servers[server_index]->user_names[channel_index][name_index]);
										servers[server_index]->user_names[channel_index][name_index]=NULL;
									}
								}
							}
						}
					}
				//":accirc!1@hide-68F46812.device.mst.edu NICK :accirc_2"
				//handle for NICK changes, especially the special case of our own, where server[server_index]->nick should get reset
				//NICK changes are server-wide so I'll only be able to handle this better once I have a list of users in each channel
				}else if(!strcmp(command,"NICK")){
					char our_nick_changed=FALSE;
					//if we changed our nick
					if(!strcmp(nick,servers[server_index]->nick)){
						//change it in relevant data structures
						//leaving out the leading ":"
						substr(servers[server_index]->nick,text,1,strlen(text)-1);
						//and update the display to reflect this change
						refresh_server_list();
						our_nick_changed=TRUE;
					}
					
					//set the user's nick to be lower-case for case-insensitive string matching
					strtolower(nick,BUFFER_SIZE);
					
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							int name_index;
							for(name_index=0;name_index<MAX_NAMES;name_index++){
								if(servers[server_index]->user_names[channel_index][name_index]!=NULL){
#ifdef DEBUG
//									char really_really_tmp[BUFFER_SIZE];
//									sprintf(really_really_tmp,"NICK debug 0, trying name \"%s\"",servers[server_index]->user_names[channel_index][name_index]);
//									scrollback_output(server_index,0,really_really_tmp);
#endif
									
									char this_name[BUFFER_SIZE];
									strncpy(this_name,servers[server_index]->user_names[channel_index][name_index],BUFFER_SIZE);
									strtolower(this_name,BUFFER_SIZE);
									
									//found it!
									if(!strcmp(this_name,nick)){
										//output to the appropriate channel
										scrollback_output(server_index,channel_index,output_buffer);
										
										char new_nick[BUFFER_SIZE];
										substr(new_nick,text,1,strlen(text)-1);
										
										//update this user's entry in that channel's names array
										strncpy(servers[server_index]->user_names[channel_index][name_index],new_nick,BUFFER_SIZE);
									}
								}
							}
						}
					}
					
					//if it was us go ahead and output to the reserved server channel also
					if(our_nick_changed){
						special_output=FALSE;
					}else{
						//we output here already so don't output at the end
						special_output=TRUE;
					}
				//handle for topic changes
				//":accirc_user!1@hide-68F46812.device.mst.edu TOPIC #FaiD3.0 :Welcome to #winfaid 4.0, now with grammar checking"
				}else if(!strcmp(command,"TOPIC")){
					char channel[BUFFER_SIZE];
					int space_colon_index=strfind(" :",tmp_buffer);
					substr(channel,tmp_buffer,0,space_colon_index);
					
					substr(tmp_buffer,tmp_buffer,space_colon_index+2,strlen(tmp_buffer)-space_colon_index-2);
					
					strncpy(text,tmp_buffer,BUFFER_SIZE);
					
					//lower case the channel so we can do a case-insensitive string match against it
					strtolower(channel,BUFFER_SIZE);
					
					//go through the channels, find out the one to output to, set "output_channel" to that index
					//note that if we never find the channel output_channel stays at its default, which is the SERVER channel
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							char lower_case_channel[BUFFER_SIZE];
							strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
							strtolower(lower_case_channel,BUFFER_SIZE);
							
							if(!strcmp(channel,lower_case_channel)){
								//reset topic information for that channel
								strncpy(servers[server_index]->channel_topic[channel_index],text,BUFFER_SIZE);
								//and output
								refresh_channel_topic();
								
								output_channel=channel_index;
								channel_index=MAX_CHANNELS;
							}
						}
					}
				//":Shishichi!notIRCuser@hide-4C94998D.fidnet.com MODE #FaiD3.0 +o MonkeyofDoom"
				}else if(!strcmp(command,"MODE")){
					char channel[BUFFER_SIZE];
					substr(channel,text,0,strfind(" ",text));
					
					//lower case the channel so we can do a case-insensitive string match against it
					strtolower(channel,BUFFER_SIZE);
					//go through the channels, find out the one to output to, set "output_channel" to that index
					//note that if we never find the channel output_channel stays at its default, which is the SERVER channel
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							char lower_case_channel[BUFFER_SIZE];
							strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
							strtolower(lower_case_channel,BUFFER_SIZE);
							
							if(!strcmp(channel,lower_case_channel)){
								output_channel=channel_index;
								channel_index=MAX_CHANNELS;
							}
						}
					}
				//TODO: add proper NOTICE handling
				}else if(!strcmp(command,"NOTICE")){
					
				//using channel names lists, output quits to the correct channel
				//(this will require outputting multiple times, which I don't have the faculties for at the moment)
				}else if(!strcmp(command,"QUIT")){
					//set the user's nick to be lower-case for case-insensitive string matching
					strtolower(nick,BUFFER_SIZE);
					
					int channel_index;
					for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
						if(servers[server_index]->channel_name[channel_index]!=NULL){
							int name_index;
							for(name_index=0;name_index<MAX_NAMES;name_index++){
								if(servers[server_index]->user_names[channel_index][name_index]!=NULL){
									char this_name[BUFFER_SIZE];
									strncpy(this_name,servers[server_index]->user_names[channel_index][name_index],BUFFER_SIZE);
									strtolower(this_name,BUFFER_SIZE);
									
									//found it!
									if(!strcmp(this_name,nick)){
										//output to the appropriate channel
										scrollback_output(server_index,channel_index,output_buffer);
										
										//remove this user from that channel's names array
										free(servers[server_index]->user_names[channel_index][name_index]);
										servers[server_index]->user_names[channel_index][name_index]=NULL;
										
										//for handling later; just let us know we found a channel to output to
										output_channel=channel_index;
									}
								}
							}
						}
					}
					
					//if we found the channel this user was in
					if(output_channel!=0){
						//don't output this to the system channel
						special_output=TRUE;
					}
				}
			}
		}
		
		//if we haven't already done some crazy kind of output
		if(!special_output){
			//do the normal kind of output
			scrollback_output(server_index,output_channel,output_buffer);
		}
	}
	
	//if we didn't close the connection up there
	if(servers[server_index]!=NULL){
		//clear out the real buffer for the next line from the server
		for(n=0;n<BUFFER_SIZE;n++){
			servers[server_index]->read_buffer[n]='\0';
		}
	}
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
	}
	
	//restart ncurses interface
	endwin();
	refresh();
	clear();
	//set some common options
	noecho();
	if(has_colors()){
//		start_color();
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
	}
	//get raw input
	raw();
	//set the correct terminal size constraints before we go crazy and allocate windows with the wrong ones
	getmaxyx(stdscr,height,width);
	
	if((height<MIN_HEIGHT)||(width<MIN_WIDTH)){
		endwin();
		fprintf(stderr,"Err: Window too small, would segfault if I stayed, exiting...");
		exit(1);
	}
	
	//allocate windows for our toolbars and the main chat
	server_list=newwin(1,width,0,0);
	channel_list=newwin(1,width,1,0);
	channel_topic=newwin(1,width,2,0);
	top_border=newwin(1,width,3,0);
	channel_text=newwin((height-6),width,4,0);
	bottom_border=newwin(1,width,(height-2),0);
	user_input=newwin(1,width,(height-1),0);
	
/*	//set sane default colors
	wcoloron(server_list,MIRC_WHITE,MIRC_BLACK);
	wcoloron(channel_list,MIRC_WHITE,MIRC_BLACK);
	wcoloron(channel_topic,MIRC_WHITE,MIRC_BLACK);
	wcoloron(top_border,MIRC_WHITE,MIRC_BLACK);
	wcoloron(channel_text,MIRC_WHITE,MIRC_BLACK);
	wcoloron(bottom_border,MIRC_WHITE,MIRC_BLACK);
	wcoloron(user_input,MIRC_WHITE,MIRC_BLACK);
*/
	
	keypad(user_input,TRUE);
	//set timeouts for non-blocking
	timeout(1);
	wtimeout(user_input,5);
	
	wclear(server_list);
	wclear(channel_list);
	wclear(channel_topic);
	wclear(top_border);
	wclear(channel_text);
	wclear(bottom_border);
	wclear(user_input);
	
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
	long unsigned int old_time=(uintmax_t)(time(NULL));
	sprintf(time_buffer,"[%lu]",old_time);
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

//runtime
int main(int argc, char *argv[]){
	//handle special argument cases like --version, --help, etc.
	if(argc>1){
		if(!strcmp(argv[1],"--version")){
			printf("accidental_irc, the accidental irc client, version %s\n",VERSION);
			exit(0);
		}else if(!strcmp(argv[1],"--help")){
			printf("accidental_irc, the irc client that accidentally got written; see man page for docs\n");
			exit(0);
		}
	}
	
	//store logs in ~/.local/share/accirc/logs/
	//ensure appropriate directories exist for config and logs
	char log_dir[BUFFER_SIZE];
	char *home_dir=getenv("HOME");
	if(home_dir!=NULL){
		sprintf(log_dir,"%s/.local/",home_dir);
		verify_or_make_dir(log_dir);
		sprintf(log_dir,"%s/.local/share",home_dir);
		verify_or_make_dir(log_dir);
		sprintf(log_dir,"%s/.local/share/accirc",home_dir);
		verify_or_make_dir(log_dir);
		sprintf(log_dir,"%s/.local/share/accirc/%s",home_dir,LOGGING_DIRECTORY);
	}else{
		sprintf(log_dir,LOGGING_DIRECTORY);
	}
	
	//TODO: make not making log dir a non-fatal error
	if(!verify_or_make_dir(log_dir)){
		fprintf(stderr,"Err: Could not find or create the log directory\n");
		exit(1);
	}
	
	//initialize the global variables appropriately
	int n;
	for(n=0;n<MAX_SERVERS;n++){
		servers[n]=NULL;
	}
	
	//location in input history, starting at "not looking at history" state
	input_line=-1;
	//location in scrollback for the current channel, -1 meaning not scrolled back at all (and hence new messages are displayed as they come in)
	scrollback_end=-1;
	
	//negative values mean the user is not connected to anything (this is the default)
	current_server=-1;
	
	//declare some variables
	//for the clock
	char time_buffer[BUFFER_SIZE];
	//for user input
	char input_buffer[BUFFER_SIZE];
	for(n=0;n<BUFFER_SIZE;n++){
		input_buffer[n]='\0';
		time_buffer[BUFFER_SIZE]='\0';
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
	
	//force a re-detection of the window and a re-allocation of resources
	force_resize("",0,0);
	
	//store config in ~/.config/accirc/config.rc
	char rc_file[BUFFER_SIZE];
	//char *home_dir=getenv("HOME");
	if(home_dir!=NULL){
		sprintf(rc_file,"%s/.config",home_dir);
		verify_or_make_dir(rc_file);
		sprintf(rc_file,"%s/.config/accirc",home_dir);
		verify_or_make_dir(rc_file);
		sprintf(rc_file,"%s/.config/accirc/config.rc",home_dir);
	}else{
		sprintf(rc_file,"config.rc");
	}
	
	//if this fails no rc will be used
	load_rc(rc_file);
	
	//start the clock
	long unsigned int old_time=(uintmax_t)(time(NULL));
	
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
	int c;
	//determine if we're done
	done=FALSE;
	//main loop
	while(!done){
		//store what the current_server and channel in that server were previously so we know if they change
		//if they change we'll update the display
		int old_server=current_server;
		int old_channel=-1;
		if(current_server>=0){
			old_channel=servers[current_server]->current_channel;
		}
		
		//store the previous input buffer so we know if it changed
		char old_input_buffer[BUFFER_SIZE];
		strncpy(old_input_buffer,input_buffer,BUFFER_SIZE);
		
		c=wgetch(user_input);
		if(c>=0){
			switch(c){
				//handle for resize events
				case KEY_RESIZE:
					//if we were scrolled back we're not anymore!
					scrollback_end=-1;
					force_resize(input_buffer,cursor_pos,input_display_start);
					break;
				//TODO: make another break command or change something else to add ^C to the buffer, it's needed for MIRC colors
				//handle ctrl+c gracefully
//				case BREAK:
//					done=TRUE;
//					break;
				//TODO: get the proper escape sequence for these, TEMPORARILY they are using f1,f2,f3,f4
				//these are ALT+arrows to move between channels and servers
//				case ALT_UP:
				//f3
				case 267:
					strncpy(key_combo_buffer,"/sl",BUFFER_SIZE);
					parse_input(key_combo_buffer,FALSE);
					break;
//				case ALT_DOWN:
				//f4
				case 268:
					strncpy(key_combo_buffer,"/sr",BUFFER_SIZE);
					parse_input(key_combo_buffer,FALSE);
					break;
//				case ALT_LEFT:
				//f1
				case 265:
					strncpy(key_combo_buffer,"/cl",BUFFER_SIZE);
					parse_input(key_combo_buffer,FALSE);
					break;
//				case ALT_RIGHT:
				//f2
				case 266:
					strncpy(key_combo_buffer,"/cr",BUFFER_SIZE);
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
						if(cursor_pos>width){
							input_display_start++;
						}
					}
					refresh_user_input(input_buffer,cursor_pos,input_display_start);
					break;
				case KEY_LEFT:
					if(cursor_pos>0){
						cursor_pos--;
						if(cursor_pos<input_display_start){
							input_display_start--;
						}
					}
					refresh_user_input(input_buffer,cursor_pos,input_display_start);
					break;
				//scroll back in the current channel
				//TODO: compute correct stop scrolling bound in this case
//				case KEY_PGUP:
				case 339:
					//if we are connected to a server
					if(current_server>=0){
						int line_count;
						char **scrollback=servers[current_server]->channel_content[servers[current_server]->current_channel];
						for(line_count=0;(line_count<MAX_SCROLLBACK)&&(scrollback[line_count]!=NULL);line_count++);
						
						//if there is more text than area to display allow it scrollback (else don't)
						//the -6 here is because there are 6 character rows used to display things other than channel text
//						if(line_count>height-6){
						if(line_count>0){
							//if we're already scrolled back and we can go further
							//note: the +6 here is because there are 6 character rows used to display things other than channel text
//							if((scrollback_end-height+6)>0){
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
					break;
				//scroll forward in the current channel
//				case KEY_PGDN:
				case 338:
					//if we are connected to a server
					if(current_server>=0){
						char **scrollback=servers[current_server]->channel_content[servers[current_server]->current_channel];
						//if we're already scrolled back and there is valid scrollback below this
						if((scrollback_end>=0)&&(scrollback_end<(MAX_SCROLLBACK-1))&&(scrollback[scrollback_end+1]!=NULL)){
							scrollback_end++;
						//if we're out of scrollback to view, re-set and make this display new data as it gets here
						}else if(scrollback_end>=0){
							scrollback_end=-1;
						}
						refresh_channel_text();
					}
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
					if(current_server>=0){
						//a portion of the nickname to complete
						char partial_nick[BUFFER_SIZE];
						//where the nickname starts
						int partial_nick_start_index=(cursor_pos-1);
						while((partial_nick_start_index>=0)&&(input_buffer[partial_nick_start_index]!=' ')){
							partial_nick_start_index--;
						}
						//don't count the delimeter
						partial_nick_start_index++;
						
						//chomp up the nickname start
						int n;
						for(n=partial_nick_start_index;n<cursor_pos;n++){
							partial_nick[n-partial_nick_start_index]=input_buffer[n];
						}
						//always null terminate
						partial_nick[n]='\0';
						
						//lower case for case-insensitive matching
						strtolower(partial_nick,BUFFER_SIZE);
						
						//this counts the number of matches we got, we only want to complete on UNIQUE matches
						int matching_nicks=0;
						char last_matching_nick[BUFFER_SIZE];
						//iterate through all tne nicks in this channel, if we find a unique match, complete it
						for(n=0;n<MAX_NAMES;n++){
							if(servers[current_server]->user_names[servers[current_server]->current_channel][n]!=NULL){
								char nick_to_match[BUFFER_SIZE];
								strncpy(nick_to_match,servers[current_server]->user_names[servers[current_server]->current_channel][n],BUFFER_SIZE);
								strtolower(nick_to_match,BUFFER_SIZE);
								
								//if this nick started with the partial_nick
								if(strfind(partial_nick,nick_to_match)==0){
									matching_nicks++;
									strncpy(last_matching_nick,servers[current_server]->user_names[servers[current_server]->current_channel][n],BUFFER_SIZE);
								}
							}
						}
						
						//if this was a unique match
						if(matching_nicks==1){
							//fill in the rest of the name
							//where to start inserting from in the full nick
							int insert_start_pos=cursor_pos-partial_nick_start_index;
							
							int n;
							for(n=insert_start_pos;n<strlen(last_matching_nick);n++){
								if(strinsert(input_buffer,last_matching_nick[n],cursor_pos,BUFFER_SIZE)){
									cursor_pos++;
									//if we would go off the end
									if((cursor_pos-input_display_start)>width){
										//make the end one char further down
										input_display_start++;
									}
								}
							}
						//this was either not a match or not a unique match
						}else{
							beep();
						}
					}
					break;
				//TODO: make ctrl+tab actually send a tab
				case KEY_HOME:
					cursor_pos=0;
					input_display_start=0;
					refresh_user_input(input_buffer,cursor_pos,input_display_start);
					break;
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
				//TODO: make this ctrl+tab if possible, or something else that makes more sense
				//temporarily f5 sends a literal tab
				case 269:
				//and f6 sends a 0x01 (since screen catches the real one)
				case 270:
					//these are mutually exclusive, so an if is needed
					if(c==269){
						c='\t';
					}else if(c==270){
						c=0x01;
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
					
#ifdef DEBUG
					if(current_server<0){
						wclear(channel_text);
						wmove(channel_text,0,0);
						wprintw(channel_text,"%i",c);
						wrefresh(channel_text);
					}
#endif
					break;
			}
		}
		
		//loop through servers and see if there's any data worth reading
		int server_index;
		for(server_index=0;server_index<MAX_SERVERS;server_index++){
			//if this is a valid server connection we have a buffer allocated
			if(servers[server_index]!=NULL){
				//can't make a more descriptive name than that; go ahead, I dare you to try
				char one_byte_buffer[1];
				int bytes_transferred=recv(servers[server_index]->socket_fd,one_byte_buffer,1,0);
				if((bytes_transferred<=0)&&(errno!=EAGAIN)){
					//TODO: handle connection errors gracefully here
					properly_close(server_index);
				}else if(bytes_transferred>0){
					//add this byte to the total buffer
					if(strinsert(servers[server_index]->read_buffer,one_byte_buffer[0],strlen(servers[server_index]->read_buffer),BUFFER_SIZE)){
						//a newline ends the reading and makes us start from scratch for the next byte
						if(one_byte_buffer[0]=='\n'){
							//parse a line from the server, with the only relevant information needed being the server_index
							//(everything else needed is global)
							parse_server(server_index);
						}//else do nothing, we'll handle it when we get some more bytes
					//oh shit, we overflowed the buffer
					}else{
						//TODO: handle this more gracefully than just clearing the buffer and ignoring what was in it
						int n;
						for(n=0;n<BUFFER_SIZE;n++){
							servers[server_index]->read_buffer[n]='\0';
						}
					}
				}
			}
		}
		
		//unix epoch clock in bottom_border, update it when the time changes
		long unsigned int current_time=(uintmax_t)(time(NULL));
		//if the time has changed
		if(current_time>old_time){
			wclear(bottom_border);
			
			sprintf(time_buffer,"[%lu]",old_time);
			wmove(bottom_border,0,0);
			wprintw(bottom_border,time_buffer);
			
			for(n=strlen(time_buffer);n<width;n++){
				wprintw(bottom_border,"-");
			}
			//refresh the window from the buffer
			wrefresh(bottom_border);
			
			//re-set for next iteration
			old_time=current_time;
			
			//make sure the user doesn't see their cursor move
			wrefresh(user_input);
		}
		
		//output the most up-to-date information about servers, channels, topics, and various whatnot
		//(do this where changes occur so we're not CONSTANTLY refreshing, which causes flicker among other things)
		if((old_server!=current_server)&&(current_server>=0)){
			refresh_server_list();
			refresh_channel_list();
			refresh_channel_topic();
			refresh_channel_text();
		//if the server didn't change but the channel did, still update the channel text
		}else if((current_server>=0)&&(old_channel!=(servers[current_server]->current_channel))){
			refresh_channel_list();
			refresh_channel_topic();
			refresh_channel_text();
		}
		
		//the cursor is one thing that should ALWAYS be visible
		wmove(user_input,0,cursor_pos);
		
		//if the user typed something
		if(strcmp(input_buffer,old_input_buffer)!=0){
			//if the string is not as wide as the display allows re-set the input display starting point to show the whole string always
			if(strlen(input_buffer)<width){
				input_display_start=0;
			}
			refresh_user_input(input_buffer,cursor_pos,input_display_start);
		}
	}
	
	//free all the RAM we allocated for anything
	int server_index;
	for(server_index=0;server_index<MAX_SERVERS;server_index++){
		//if this is a valid server connection
		if(servers[server_index]!=NULL){
			properly_close(server_index);
		}
	}
	
	//end ncurses cleanly
	endwin();
	
	return 0;
}

