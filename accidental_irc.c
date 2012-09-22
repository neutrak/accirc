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

#define VERSION 0.1

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
#define MAX_SCROLLBACK 200

//some defaults in case the user forgets to give a nickname
#define DEFAULT_NICK "accidental_irc_user"

//and in case the user doesn't give a proper quit message
#define DEFAULT_QUIT_MESSAGE "accidental_irc exited"

//the error to display when a line overflows
#define LINE_OVERFLOW_ERROR "<<etc.>>"

//the default directoryto save logs to
#define LOGGING_DIRECTORY "logs"

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
	
	//this data is what the user sees (but is also used for other things)
	char server_name[BUFFER_SIZE];
	char nick[BUFFER_SIZE];
	char *channel_name[MAX_CHANNELS];
	char **channel_content[MAX_CHANNELS];
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

//display server list updates as needed; bolding the current server
void refresh_server_list(){
	//update the display of the server list
	wclear(server_list);
	wmove(server_list,0,0);
	int n;
	for(n=0;n<MAX_SERVERS;n++){
		//if the server is connected
		if(servers[n]!=NULL){
			//TODO: fix the new_server_content handling, right now this line is temporary to ignore it entirely
			servers[current_server]->new_server_content=FALSE;
			
			//if it's the active server bold it
			if(current_server==n){
				wattron(server_list,A_BOLD);
				wprintw(server_list,servers[n]->server_name);
				wattroff(server_list,A_BOLD);
				
				//if we're viewing this server any content that would be considered "new" is no longer there
				servers[current_server]->new_server_content=FALSE;
			//else if there is new data, display differently to show that to the user
			}else if(servers[current_server]->new_server_content==TRUE){
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
	//print out the channel topic
	//first clearing that window
	wclear(channel_topic);
	
	//start at the start of the line
	wmove(channel_topic,0,0);
	wprintw(channel_topic,servers[current_server]->channel_topic[servers[current_server]->current_channel]);
	
	//refresh the channel topic window
	wrefresh(channel_topic);
}

//display channel text as needed using current server and severs[current_server]->current_channel to tell what to display
void refresh_channel_text(){
	//number of lines of scrollback available
	int line_count;
	char **scrollback=servers[current_server]->channel_content[servers[current_server]->current_channel];
	for(line_count=0;(line_count<MAX_SCROLLBACK)&&(scrollback[line_count]!=NULL);line_count++);
	
	int w_height,w_width;
	getmaxyx(channel_text,w_height,w_width);
	
	//if there's not enough lines to fill the whole window
	if(line_count<w_height){
		//fill a sub-window only
		w_height=line_count;
	}
	
	//print out the channel text
	//first clearing that window
	wclear(channel_text);
	
	//where to stop outputting, by default this is the last line available
	int output_end=line_count;
	
	if(scrollback_end>=0){
		output_end=scrollback_end;
	}
	
	//TODO: word wrap, and do it sanely (for the moment I'm just giving a line overflow error, which is not /terrible/ but not /great/ either)
	int output_line;
	for(output_line=(output_end-w_height);output_line<output_end;output_line++){
		//if there's text to display on this line
		if(scrollback[output_line]!=NULL){
			//start at the start of the line
			wmove(channel_text,output_line-(output_end-w_height),0);
			if(strlen(scrollback[output_line])<w_width){
				wprintw(channel_text,scrollback[output_line]);
			}else{
				//NOTE: although we're not outputting the full line here, the full line WILL be in the logs for the user to view
				//and WILL be in the scrollback should the user resize the window
				char output_text[BUFFER_SIZE];
				int n;
				for(n=0;n<w_width;n++){
					output_text[n]=scrollback[output_line][n];
				}
				char line_overflow_error[BUFFER_SIZE];
				strncpy(line_overflow_error,LINE_OVERFLOW_ERROR,BUFFER_SIZE);
				for(n=w_width-strlen(line_overflow_error);n<w_width;n++){
					output_text[n]=line_overflow_error[n-w_width+strlen(line_overflow_error)];
				}
				output_text[w_width]='\0';
				wprintw(channel_text,output_text);
			}
		}
	}
	//refresh the channel text window
	wrefresh(channel_text);
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
	//TODO: these are mutually exclusive, see if I can get by with just one flag
	char server_command=FALSE;
	char client_command=FALSE;
	
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
				//TODO: handle for insufficient parameters
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
					fprintf(stderr,"Err: Could not open socket\n");
#endif
					goto fail;
				}
				
				//that's "get host by name"
				struct hostent *server=gethostbyname(host);
				if(server==NULL){
					//TODO: handle failed hostname lookups gracefully here
#ifdef DEBUG
					fprintf(stderr,"Err: Could not find server\n");
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
					fprintf(stderr,"Err: Could not connect to server\n");
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
						
						//TODO: make keeping logs a setting, not just always true
						servers[server_index]->keep_logs=TRUE;
						if(servers[server_index]->keep_logs){
							//first make a directory for this server
							char file_location[BUFFER_SIZE];
							sprintf(file_location,"%s/%s",LOGGING_DIRECTORY,servers[server_index]->server_name);
							if(verify_or_make_dir(file_location)){
								sprintf(file_location,"%s/%s/%s",LOGGING_DIRECTORY,servers[server_index]->server_name,servers[server_index]->channel_name[0]);
								//note that if this call fails it will be set to NULL and hence be skipped over when writing logs
								servers[server_index]->log_file[0]=fopen(file_location,"a");
							//TODO: make this fail in a non-silent way, the user should know there was a problem
							//if we couldn't make the directory then don't keep logs rather than failing hard
							}else{
								servers[server_index]->keep_logs=FALSE;
							}
						}
						
						//NULL out all other channels
						//note this starts from 1 since 0 is the SERVER channel
						for(n=1;n<MAX_CHANNELS;n++){
							servers[server_index]->channel_name[n]=NULL;
							servers[server_index]->channel_content[n]=NULL;
							servers[server_index]->channel_topic[n]=NULL;
							servers[server_index]->new_channel_content[n]=FALSE;
							servers[server_index]->log_file[n]=NULL;
						}
						
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
		}
	//if it's a server command send the raw text to the server
	}else if(server_command){
		char to_send[BUFFER_SIZE];
		sprintf(to_send,"%s\n",input_buffer);
		safe_send(servers[current_server]->socket_fd,to_send);
		
		//format the text for my viewing benefit (this is also what will go in logs, with a newline)
		char output_buffer[BUFFER_SIZE];
		sprintf(output_buffer,"%lu %s",(uintmax_t)(time(NULL)),input_buffer);
		
		//place my own text in the scrollback for this server and channel
		
		//add the message to the relevant channel scrollback structure (note this is always output to the SERVER channel)
		char **scrollback=servers[current_server]->channel_content[0];
		
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
		
		strncpy(scrollback[scrollback_line],output_buffer,BUFFER_SIZE);
		
		//if we're keeping logs write to them
		int current_channel=0;
		if((servers[current_server]->keep_logs)&&(servers[current_server]->log_file[current_channel]!=NULL)){
			fprintf(servers[current_server]->log_file[current_channel],"%s\n",output_buffer);
		}
		
		//set the new content flag for the channel list to notice it
		servers[current_server]->new_channel_content[0]=TRUE;
		
		//refresh the channel list because there is new content
		refresh_channel_list();
		
		//refresh the channel text just in case
		refresh_channel_text();
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
				sprintf(output_buffer,">> %lu <%s> %s",(uintmax_t)(time(NULL)),servers[current_server]->nick,input_buffer);
				
				//place my own text in the scrollback for this server and channel
				
				//add the message to the relevant channel scrollback structure
				char **scrollback=servers[current_server]->channel_content[servers[current_server]->current_channel];
				
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
				
				//NOTE: this is an output buffer and not the raw input buffer because we might not always output /exactly/ what we got from the user
				//but we don't want the input_buffer changed above
//				strncpy(scrollback[scrollback_line],input_buffer,BUFFER_SIZE);
				strncpy(scrollback[scrollback_line],output_buffer,BUFFER_SIZE);
				
				//if we're keeping logs write to them
				int current_channel=servers[current_server]->current_channel;
				if((servers[current_server]->keep_logs)&&(servers[current_server]->log_file[current_channel]!=NULL)){
					fprintf(servers[current_server]->log_file[current_channel],"%s\n",output_buffer);
				}
				
				//refresh the display, since we're always going to be on the current channel when this happens
				refresh_channel_text();
			}
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
	}else{
		//take out the trailing newline (accounting for the possibility of windows newlines
		int newline_index=strfind("\r\n",servers[server_index]->read_buffer);
		if(newline_index<0){
			newline_index=strfind("\n",servers[server_index]->read_buffer);
		}
		//NOTE: I can set this to be a substring of itself since I'm not overwriting anything during copy that I'll need
		substr(servers[server_index]->read_buffer,servers[server_index]->read_buffer,0,newline_index);
		
		//set this to show as having new data, it must since we're getting something on it
		servers[server_index]->new_server_content=TRUE;
		
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
			//if this message started with the server's name
			if(!strcmp(command,servers[server_index]->server_name)){
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
				//current channel topic and associated information that uses a similar format (so I don't have to re-write code)
				}else if((!strcmp(command,"332"))||(!strcmp(command,"333"))){
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
								if(!strcmp(command,"332")){
									sprintf(output_buffer,"TOPIC for %s :%s",channel,topic);
									
									//store the topic in the general data structure
									strncpy(servers[server_index]->channel_topic[channel_index],topic,BUFFER_SIZE);
									
									//and output
									refresh_channel_topic();
								//this is who set the topic and when
								}else if(!strcmp(command,"333")){
									sprintf(output_buffer,"topic set information :%s",topic);
								}
								
								channel_index=MAX_CHANNELS;
							}
						}
					}
				//end of message of the day (useful as a delimeter)
				}else if(!strcmp(command,"376")){
					
				}
			//default is of the form ":neutrak!neutrak@hide-F99E0499.device.mst.edu PRIVMSG accirc_user :test"
			//or ":accirc_2!1@hide-68F46812.device.mst.edu JOIN :#FaiD3.0"
			//or ":accirc!1@hide-68F46812.device.mst.edu NICK :accirc_2"
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
				if(!strcmp(command,"PRIVMSG")){
					char channel[BUFFER_SIZE];
					int space_colon_index=strfind(" :",tmp_buffer);
					substr(channel,tmp_buffer,0,space_colon_index);
					
					substr(tmp_buffer,tmp_buffer,space_colon_index+2,strlen(tmp_buffer)-space_colon_index-2);
					
					strncpy(text,tmp_buffer,BUFFER_SIZE);
					
					//lower case the channel so we can do a case-insensitive string match against it
					for(n=0;n<BUFFER_SIZE;n++){
						if(channel[n]!='\0'){
							channel[n]=tolower(channel[n]);
						}else{
							n=BUFFER_SIZE;
						}
					}
					
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
					//TODO: handle CTCP ACTION, VERSION, and PING; also MIRC colors
					
					//handle for a ping (when someone says our own nick)
					int name_index=strfind(servers[server_index]->nick,text);
					if(name_index>=0){
						//TODO: take any desired additional steps upon ping here (notify-send or something)
						//format the output to show that we were pingged
						sprintf(output_buffer,"%lu ***<%s> %s",(uintmax_t)(time(NULL)),nick,text);
					}else{
						//format the output of a PM in a very pretty way
						sprintf(output_buffer,"%lu <%s> %s",(uintmax_t)(time(NULL)),nick,text);
					}
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
							
							//if we should be keeping logs make sure we are
							if(servers[server_index]->keep_logs){
								char file_location[BUFFER_SIZE];
								sprintf(file_location,"%s/%s/%s",LOGGING_DIRECTORY,servers[server_index]->server_name,servers[server_index]->channel_name[channel_index]);
								//note if this fails it will be set to NULL and hence will be skipped over when trying to output to it
								servers[server_index]->log_file[channel_index]=fopen(file_location,"a");
								
#ifdef DEBUG
								if(servers[server_index]->log_file[channel_index]!=NULL){
									//turn off buffering since I need this output for debugging and buffers annoy me for that
									setvbuf(servers[server_index]->log_file[channel_index],NULL,_IONBF,0);
								}
#endif
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
						//lower case the channel so we can do a case-insensitive string match against it
						strtolower(text,BUFFER_SIZE);
						
						int channel_index;
						for(channel_index=0;channel_index<MAX_CHANNELS;channel_index++){
							if(servers[server_index]->channel_name[channel_index]!=NULL){
								char lower_case_channel[BUFFER_SIZE];
								strncpy(lower_case_channel,servers[server_index]->channel_name[channel_index],BUFFER_SIZE);
								strtolower(lower_case_channel,BUFFER_SIZE);
								
								if(!strcmp(lower_case_channel,text)){
									output_channel=channel_index;
									channel_index=MAX_CHANNELS;
								}
							}
						}
					}
				//TODO: handle for NICK changes, especially the special case of our own, where server[server_index]->nick should get reset
				//NICK changes are server-wide so I'll only be able to handle this better once I have a list of users in each channel
				}else if(!strcmp(command,"NICK")){
					//if we changed our nick
					if(!strcmp(nick,servers[server_index]->nick)){
						//change it in relevant data structures
						//leaving out the leading ":"
						substr(servers[server_index]->nick,text,1,strlen(text)-1);
						//and update the display to reflect this change
						refresh_server_list();
					}
				}
			}
		}
		
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
//		strncpy(scrollback[scrollback_line],servers[server_index]->read_buffer,BUFFER_SIZE);
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
	
	//clear out the real buffer for the next line from the server
	for(n=0;n<BUFFER_SIZE;n++){
		servers[server_index]->read_buffer[n]='\0';
	}
}

//force resize detection
void force_resize(char *input_buffer, int cursor_pos){
	//de-allocate existing windows so as not to waste RAM
	delwin(server_list);
	delwin(channel_list);
	delwin(channel_topic);
	delwin(top_border);
	delwin(channel_text);
	delwin(bottom_border);
	delwin(user_input);
	
	//restart ncurses interface
	endwin();
	refresh();
	clear();
	//set some common options
	noecho();
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
	
	//output the most recent text from the user so they can see what they're typing
	wclear(user_input);
	wmove(user_input,0,0);
	wprintw(user_input,"%s",input_buffer);
	wmove(user_input,0,cursor_pos);
	wrefresh(user_input);
}

//runtime
int main(int argc, char *argv[]){
	//handle special argument cases like --version, --help, etc.
	if(argc>1){
		if(!strcmp(argv[1],"--version")){
			printf("accidental_irc version %f\n",VERSION);
			exit(0);
		}else if(!strcmp(argv[1],"--help")){
			//TODO: make a man page
			printf("accidental_irc, the irc client that accidentally got written; MAN PAGE COMING SOON!\n");
			exit(0);
		}
	}
	
	//TODO: store logs in ~/.local/share/accirc/logs/
	//ensure appropriate directories exist for config and logs
	if(!verify_or_make_dir("logs")){
		fprintf(stderr,"Err: Could not find or create the log directory\n");
		exit(1);
	}
	
	//TODO: store config in ~/.config/accirc/config.ini
//	if(!verify_or_make_dir("accirc")){
//		fprintf(stderr,"Err: Could not find or create the config directory\n");
//		exit(1);
//	}
	char *config_file="config.ini";
	
	FILE *settings=fopen(config_file,"r");
	if(!settings){
		fprintf(stderr,"Warn: Settings file not found, assuming defaults\n");
	}else{
		//TODO: read in the ini settings file
		fclose(settings);
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
	clear();
	//set some common options
	noecho();
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
	
	wprintw(server_list,"(no servers)");
	wprintw(channel_list,"(no channels)");
	wprintw(channel_topic,"(no channel topic)");
	wprintw(channel_text,"(no channel text)");
	
	for(n=0;n<width;n++){
		wprintw(top_border,"-");
	}
	
	//unix epoch clock (initialization)
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
	
	//start the cursor in the user input area
	wmove(user_input,0,0);
	wrefresh(user_input);
	
	//a one-buffer long history for when the user is scrolling back into the real history
	//heh, pre-history; this is the dino-buffer, :P
	char pre_history[BUFFER_SIZE];
	
	//the current position in the string, starting at 0
	int cursor_pos=0;
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
					force_resize(input_buffer,cursor_pos);
					break;
				//handle ctrl+c gracefully
				case BREAK:
					done=TRUE;
					break;
				//user hit enter, meaning parse and handle the user's input
				case '\n':
					//note the input_buffer gets reset to all NULL after parse_input
					parse_input(input_buffer,TRUE);
					//reset the cursor for the next round of input
					cursor_pos=0;
					break;
				//movement within the input line
				case KEY_RIGHT:
					if(cursor_pos<strlen(input_buffer)){
						cursor_pos++;
					}
					break;
				case KEY_LEFT:
					if(cursor_pos>0){
						cursor_pos--;
					}
					break;
				//scroll back in the current channel
//				case KEY_PGUP:
				case 339:
					//if we are connected to a server
					if(current_server>=0){
						int line_count;
						char **scrollback=servers[current_server]->channel_content[servers[current_server]->current_channel];
						for(line_count=0;(line_count<MAX_SCROLLBACK)&&(scrollback[line_count]!=NULL);line_count++);
						
						//if there is more text than area to display allow it scrollback (else don't)
						//the -6 here is because there are 6 character rows used to display things other than channel text
						if(line_count>height-6){
							//if we're already scrolled back and we can go further
							//note: the +6 here is because there are 6 character rows used to display things other than channel text
							if((scrollback_end-height+6)>0){
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
					break;
				//handle text entry history
				case KEY_DOWN:
					//reset cursor position always, since the strings in history are probably not the same length as the current input string
					cursor_pos=0;
					
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
					break;
				//TODO: handle user name completion (make ctrl+tab actually send a tab)
				case '\t':
					break;
				case KEY_HOME:
					cursor_pos=0;
					break;
				case KEY_END:
					cursor_pos=strlen(input_buffer);
					break;
				//this accounts for some odd-ness in terminals, it's just backspace (^H)
				case 127:
				//user wants to destroy something they entered
				case KEY_BACKSPACE:
					if(cursor_pos>0){
						if(strremove(input_buffer,cursor_pos-1)){
							//and update the cursor position upon success
							cursor_pos--;
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
				//normal input
				default:
					if(strinsert(input_buffer,(char)(c),cursor_pos,BUFFER_SIZE)){
						cursor_pos++;
					}
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
					close(servers[server_index]->socket_fd);
					
					//at the moment this just de-allocates associated memory
					int n;
					for(n=0;n<MAX_CHANNELS;n++){
						if((servers[server_index]->channel_name[n])!=NULL){
							free(servers[server_index]->channel_name[n]);
							int n1;
							for(n1=0;n1<MAX_SCROLLBACK;n1++){
								if(servers[server_index]->channel_content[n][n1]!=NULL){
									free(servers[server_index]->channel_content[n][n1]);
								}
							}
							free(servers[server_index]->channel_content[n]);
						}
					}
					free(servers[server_index]);
					//reset that entry to null for subsequent iterations
					servers[server_index]=NULL;
					
					//if we were on that server we'd better switch because it's not there anymore
					if(server_index==current_server){
						for(n=0;n<MAX_SERVERS;n++){
							if(servers[n]!=NULL){
								//reset the current server (note if there are still >1 connected servers it'll hit the last one)
								current_server=n;
							//if there are no servers connected any more
							}else if(n==(MAX_SERVERS-1)){
								//go back to the generic unconnected view
								current_server=-1;
							}
						}
					}
					
					//if there are no servers left display a depressing message
					if(current_server==-1){
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
					//if there are servers left show that
					}else{
						//update the server list display to reflect this server no longer being there (EVEN IF this wasn't the currently selected server, which is why I can't combine this with the above loop)
						refresh_server_list();
					}
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
			//output the most recent text from the user so they can see what they're typing
			wclear(user_input);
			wmove(user_input,0,0);
			wprintw(user_input,"%s",input_buffer);
			wmove(user_input,0,cursor_pos);
			wrefresh(user_input);
		}
	}
	//TODO: figure out what's segfaulting on ^C, I think it's in here somewhere
	//free all the RAM we allocated for anything
	int server_index;
	for(server_index=0;server_index<MAX_SERVERS;server_index++){
		//if this is a valid server connection
		if(servers[server_index]!=NULL){
//			safe_send(servers[server_index]->socket_fd,"QUIT :accidental_irc exited\n");
//			close(servers[server_index]->socket_fd);
			
			int n;
			for(n=0;n<MAX_CHANNELS;n++){
				if((servers[server_index]->channel_name[n])!=NULL){
					free(servers[server_index]->channel_name[n]);
					free(servers[server_index]->channel_topic[n]);
					
					int n1;
					for(n1=0;n1<MAX_SCROLLBACK;n1++){
						if(servers[server_index]->channel_content[n][n1]!=NULL){
							free(servers[server_index]->channel_content[n][n1]);
						}
					}
					free(servers[server_index]->channel_content[n]);
					
					//properly close log files
					if((servers[server_index]->keep_logs)&&(servers[server_index]->log_file[n]!=NULL)){
						fclose(servers[server_index]->log_file[n]);
					}
				}
			}
			free(servers[server_index]);
		}
	}
	
	
	//end ncurses cleanly
	endwin();
	
	return 0;
}

