//accidental irc, the accidental mutlti-server ncurses irc client
//in C at the moment (a rewrite will be done if I ever finish writing the interpreter for neulang...)

//libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
//these are for ncurses' benefit
#define KEY_ESCAPE 0x1b
#define KEY_DEL KEY_DC
#define BREAK 0x03
//(I know the IRC spec limits it to 512 but I'd like to have some extra room in case of client commands or something)
#define BUFFER_SIZE 1024
//who's gonna be on more than 64 servers at once? really?
#define MAX_SERVERS 64

//global variables
char *read_buffers[MAX_SERVERS];
int socket_fds[MAX_SERVERS];
char *nicks[MAX_SERVERS];
int current_server;
int current_channel;
WINDOW *server_list;
WINDOW *channel_list;
WINDOW *channel_topic;
WINDOW *channel_text;
WINDOW *user_input;

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

//parse user's input (note this is conextual based on current server and channel)
void parse_input(char *input_buffer){
	//ignore blank commands
	if(!strcmp("",input_buffer)){
		goto fail;
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
					goto fail;
				}
				
				//that's "get host by name"
				struct hostent *server=gethostbyname(host);
				if(server==NULL){
					//TODO: handle failed hostname lookups gracefully here
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
					goto fail;
				}
				
				//set the socket non-blocking
				//set the socket to be non-blocking
				int flags=fcntl(new_socket_fd,F_GETFL,0);
				flags=(flags==-1)?0:flags;
				fcntl(new_socket_fd,F_SETFL,flags|O_NONBLOCK);
				
				//make some data structures for relevant information
				int server_index;
				for(server_index=0;server_index<MAX_SERVERS;server_index++){
					//if this is not already a valid server
					if(read_buffers[server_index]==NULL){
						//make it one
						read_buffers[server_index]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
						nicks[server_index]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
						socket_fds[server_index]=new_socket_fd;
						
						//set the current server to be the one we just connected to
						current_server=server_index;
						
						//break this loop
						server_index=MAX_SERVERS;
					}
				}
			}
		}
	//if it's a server command send the raw text to the server
	}else if(server_command){
		char to_send[BUFFER_SIZE];
		sprintf(to_send,"%s\n",input_buffer);
		safe_send(socket_fds[current_server],to_send);
	//if it's not a command of any kind send it as a PM to current channel and server
	}else{
//		char to_send[BUFFER_SIZE];
//		sprintf(to_send,"PRIVMSG %s :%s\n",channels[current_server][current_channel],input_buffer);
//		safe_send(socket_fds[current_server],to_send);
	}
fail:	
	strncpy(input_buffer,"\0",BUFFER_SIZE);
}

//runtime
int main(int argc, char *argv[]){
	//initialize the global variables appropriately
	int n;
	for(n=0;n<MAX_SERVERS;n++){
		read_buffers[n]=NULL;
		socket_fds[n]=-1;
		nicks[n]=NULL;
	}
	
	//negative values mean the user is not connected to anything (this is the default)
	current_server=-1;
	current_channel=-1;
	
	FILE *settings=fopen("accidental_irc.ini","r");
	if(!settings){
		fprintf(stderr,"Warn: Settings file not found, assuming defaults\n");
	}else{
		//TODO: read in the ini settings file
		fclose(settings);
	}
	
	//declare some variables
	char input_buffer[BUFFER_SIZE];
	for(n=0;n<BUFFER_SIZE;n++){
		input_buffer[n]='\0';
	}
	
	int width=80;
	int height=24;
	//start ncurses interface
	initscr();
	clear();
	//set some common options
	noecho();
	//get raw input
	raw();
	//set the correct terminal size constraints before we go crazy and allocate windows with the wrong ones
	getmaxyx(stdscr,height,width);
	//allocate windows for our toolbars and the main chat
	server_list=newwin(1,width,0,0);
	channel_list=newwin(1,width,1,0);
	channel_topic=newwin(1,width,2,0);
	channel_text=newwin((height-2),width,3,0);
	user_input=newwin(1,width,(height-1),0);
	
	keypad(user_input,TRUE);
	
	wclear(server_list);
	wclear(channel_list);
	wclear(channel_topic);
	wclear(channel_text);
	wclear(user_input);
	
	wprintw(server_list,"(no servers)");
	wprintw(channel_list,"(no channels)");
	wprintw(channel_topic,"(no channel topic)");
	wprintw(channel_text,"(no channel text)");
	
	wrefresh(server_list);
	wrefresh(channel_list);
	wrefresh(channel_topic);
	wrefresh(channel_text);
	wrefresh(user_input);
	
	//start the cursor in the user input area
	wmove(user_input,0,0);
	wrefresh(user_input);
	
	//the current position in the string, starting at 0
	int cursor_pos=0;
	//one character of input
	int c;
	//determine if we're done
	char done=FALSE;
	//main loop
	while(!done){
		c=wgetch(user_input);
		
		switch(c){
			//handle ctrl+c gracefully
			case BREAK:
				done=TRUE;
				break;
			//user hit enter, meaning parse and handle the user's input
			case '\n':
				parse_input(input_buffer);
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
			case KEY_HOME:
				cursor_pos=0;
				break;
			case KEY_END:
				cursor_pos=strlen(input_buffer);
				break;
			//this accounts for some odd-ness in terminals, it's just backspace (^H)
//			case 0x08:
			//user wants to destroy something they entered
			case KEY_BACKSPACE:
				if(cursor_pos>0){
					char tmp_buffer[BUFFER_SIZE];
					//delete the character behind the specified point
					int n;
					for(n=0;n<(cursor_pos-1);n++){
						tmp_buffer[n]=input_buffer[n];
					}
					for(n=cursor_pos;n<strlen(input_buffer);n++){
						tmp_buffer[n-1]=input_buffer[n];
					}
					tmp_buffer[strlen(input_buffer)-1]='\0';
					strncpy(input_buffer,tmp_buffer,BUFFER_SIZE);
					//and update the cursor position
					cursor_pos--;
				}
				break;
			//user wants to destroy something they entered
			case KEY_DEL:
				if(cursor_pos<strlen(input_buffer)){
					char tmp_buffer[BUFFER_SIZE];
					//delete the character at the specified point
					int n;
					for(n=0;n<cursor_pos;n++){
						tmp_buffer[n]=input_buffer[n];
					}
					for(n=cursor_pos;n<strlen(input_buffer);n++){
						tmp_buffer[n]=input_buffer[n+1];
					}
					tmp_buffer[strlen(input_buffer)-1]='\0';
					strncpy(input_buffer,tmp_buffer,BUFFER_SIZE);
				}
				break;
			//normal input
			default:
				{
					char tmp_buffer[BUFFER_SIZE];
					if(cursor_pos>=strlen(input_buffer)){
						strncpy(tmp_buffer,input_buffer,BUFFER_SIZE);
						sprintf(input_buffer,"%s%c",tmp_buffer,c);
						cursor_pos=strlen(input_buffer);
					}else{
						//insert the character at the specified point
						int n;
						for(n=0;n<cursor_pos;n++){
							tmp_buffer[n]=input_buffer[n];
						}
						tmp_buffer[cursor_pos]=(char)(c);
						for(n=cursor_pos;n<strlen(input_buffer);n++){
							tmp_buffer[n+1]=input_buffer[n];
						}
						tmp_buffer[strlen(input_buffer)+1]='\0';
						strncpy(input_buffer,tmp_buffer,BUFFER_SIZE);
						//and put the cursor after that
						cursor_pos++;
					}
				}
				break;
		}
		//output the most recent text
		wclear(user_input);
		wmove(user_input,0,0);
		wprintw(user_input,"%s",input_buffer);
		wmove(user_input,0,cursor_pos);
		wrefresh(user_input);
		
		//loop through servers and see if there's any data worth reading
		int server_index;
		for(server_index=0;server_index<MAX_SERVERS;server_index++){
			//if this is a valid server connection we have a buffer allocated
			if(read_buffers[server_index]!=NULL){
				if(safe_recv(socket_fds[server_index],read_buffers[server_index])){
					//parse in whatever the server sent and display it appropriately
					int first_space=strfind(" :",read_buffers[server_index]);
					char command[BUFFER_SIZE];
					substr(command,read_buffers[server_index],0,first_space);
					if(!strcmp(command,"PING")){
						//TODO: make this less hacky than it is
						//switch the I in ping for an O in pong
						read_buffers[server_index][1]='O';
						safe_send(socket_fds[server_index],read_buffers[server_index]);
					}
					
					if(server_index==current_server){
						wprintw(channel_text,"%s",read_buffers[server_index]);
						wrefresh(channel_text);
					}
				}else if(errno!=EAGAIN){
					//TODO: handle connection errors gracefully here
					//at the moment this just de-allocates associated memory
					free(read_buffers[server_index]);
					close(socket_fds[server_index]);
					free(nicks[server_index]);
				}
			}
		}
	}
	//free all the RAM we allocated for anything
	int server_index;
	for(server_index=0;server_index<MAX_SERVERS;server_index++){
		//if this is a valid server connection we have a buffer allocated
		if(read_buffers[server_index]!=NULL){
			free(read_buffers[server_index]);
			close(socket_fds[server_index]);
			free(nicks[server_index]);
		}
	}
	
	//end ncurses cleanly
	endwin();
	
	return 0;
}

