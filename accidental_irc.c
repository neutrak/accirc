//accidental irc, the accidental mutlti-server ncurses irc client
//in C at the moment (a rewrite will be done if I ever finish writing the interpreter for neulang...)

//this takes a fractal design as follows
//	client has set of connections
//		connection has set of channels

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
//the number of lines of scrollback to store (each line being BUFFER_SIZE chars long)
#define MAX_SCROLLBACK 400

//some defaults in case the user forgets to give a nickname
#define DEFAULT_NICK "accidental_irc_user"

//global variables
//TODO: add logging, log files will probably be opened on connects and joins, and should be stored here
typedef struct irc_connection irc_connection;
struct irc_connection {
	//behind-the-scenes data, the user never sees it
	int socket_fd;
	char read_buffer[BUFFER_SIZE];
	//this data is stored in case the connection dies and we need to re-connect
	int port;
	
	//this data is what the user sees (but is also used for other things)
	char server_name[BUFFER_SIZE];
	char nick[BUFFER_SIZE];
	char *channel_name[MAX_CHANNELS];
	char **channel_content[MAX_CHANNELS];
//	char *channel_topic[MAX_CHANNELS];
	int current_channel;
};

int current_server;
irc_connection *servers[MAX_SERVERS];

//determine if we're done
char done;

WINDOW *server_list;
WINDOW *channel_list;
WINDOW *channel_topic;
WINDOW *channel_text;
WINDOW *user_input;
WINDOW *top_border;
WINDOW *bottom_border;

#ifdef DEBUG
FILE *debug_output;
#endif

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
	//TODO: add what the user input to the relevant scrollback structure in servers[current_server]
#ifdef DEBUG
	fprintf(debug_output,"parse_input debug 0, got input \"%s\"\n",input_buffer);
#endif
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
#ifdef DEBUG
		fprintf(debug_output,"parse_input debug 1\n");
#endif
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
					fprintf(debug_output,"Err: Could not open socket\n");
#endif
					goto fail;
				}
				
				//that's "get host by name"
				struct hostent *server=gethostbyname(host);
				if(server==NULL){
					//TODO: handle failed hostname lookups gracefully here
#ifdef DEBUG
					fprintf(debug_output,"Err: Could not find server\n");
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
					fprintf(debug_output,"Err: Could not connect to server\n");
#endif
					goto fail;
				}
				
				//clear out the server list because we're probably gonna be modifying it
				wclear(server_list);
				wmove(server_list,0,0);
				
				//set the socket non-blocking
				//set the socket to be non-blocking
				int flags=fcntl(new_socket_fd,F_GETFL,0);
				flags=(flags==-1)?0:flags;
				fcntl(new_socket_fd,F_SETFL,flags|O_NONBLOCK);
				
				//a flag to say if we've already added the sever
				char added=FALSE;
				
				//make some data structures for relevant information
				int server_index;
				for(server_index=0;server_index<MAX_SERVERS;server_index++){
					//if this is not already a valid server and we haven't put the new server anywhere
					if((servers[server_index]==NULL)&&(!added)){
						//make it one
						servers[server_index]=(irc_connection*)(malloc(sizeof(irc_connection)));
						//initialize the buffer to all NULL bytes
						int n;
						for(n=0;n<BUFFER_SIZE;n++){
							servers[server_index]->read_buffer[n]='\0';
						}
						servers[server_index]->socket_fd=new_socket_fd;
						
						//set the current channel to be 0 (the system/debug channel)
						//a JOIN would add channels, but upon initial connection 0 is the only valid one
						servers[server_index]->current_channel=0;
						
						//set the default channel for various messages from the server that are not channel-specific
						//NOTE: this scheme should be able to be overloaded to treat PM conversations as channels
						servers[server_index]->channel_name[0]=(char*)(malloc(BUFFER_SIZE*sizeof(char)));
						for(n=0;n<BUFFER_SIZE;n++){
							servers[server_index]->channel_name[0][n]='\0';
						}
						
						//set the main chat window with scrollback
						//as we get lines worth storing we'll add them to this content, but for the moment it's blank
						servers[server_index]->channel_content[0]=(char**)(malloc(MAX_SCROLLBACK*sizeof(char*)));
						for(n=0;n<MAX_SCROLLBACK;n++){
							servers[server_index]->channel_content[0][n]=NULL;
						}
						
						//set the port information (in case we need to re-connect)
						servers[server_index]->port=port;
						
						//set the server name
						strncpy(servers[server_index]->server_name,host,BUFFER_SIZE);
						
						//set the current server to be the one we just connected to
						current_server=server_index;
						
						//don't add this server again
						added=TRUE;
						
						//output the server information (since we set current_server to this, bold it)
						wattron(server_list,A_BOLD);
						wprintw(server_list,servers[server_index]->server_name);
						wattroff(server_list,A_BOLD);
						wprintw(server_list," | ");
						
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
		//move a server to the left
		}else if(!strcmp(command,"sl")){
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
		//move a server to the right
		}else if(!strcmp(command,"sr")){
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
		//TODO: change client, etc. commands with similar error handling and looping to the change server commands above
		
	//if it's a server command send the raw text to the server
	}else if(server_command){
		char to_send[BUFFER_SIZE];
		sprintf(to_send,"%s\n",input_buffer);
		safe_send(servers[current_server]->socket_fd,to_send);
	//if it's not a command of any kind send it as a PM to current channel and server
	}else{
//		char to_send[BUFFER_SIZE];
//		sprintf(to_send,"PRIVMSG %s :%s\n",servers[current_server]->channel_name[current_channel],input_buffer);
//		safe_send(servers[current_server]->socket_fd,to_send);
	}
fail:	
	strncpy(input_buffer,"\0",BUFFER_SIZE);
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
	
	//initialize the global variables appropriately
	int n;
	for(n=0;n<MAX_SERVERS;n++){
		servers[n]=NULL;
	}
	
	//negative values mean the user is not connected to anything (this is the default)
	current_server=-1;
	
	FILE *settings=fopen("accidental_irc.ini","r");
	if(!settings){
		fprintf(stderr,"Warn: Settings file not found, assuming defaults\n");
	}else{
		//TODO: read in the ini settings file
		fclose(settings);
	}
	
#ifdef DEBUG
	debug_output=fopen("debug_output.txt","a");
	if(!debug_output){
		fprintf(stderr,"Err: Debug output file could not be opened\n");
		exit(1);
	}
	//turn off buffering since I need this output for debugging and buffers annoy me for that
	setvbuf(debug_output,NULL,_IONBF,0);
#endif
	
	//declare some variables
	//for the clock
	char time_buffer[BUFFER_SIZE];
	//for user input
	char input_buffer[BUFFER_SIZE];
	for(n=0;n<BUFFER_SIZE;n++){
		input_buffer[n]='\0';
		time_buffer[BUFFER_SIZE]='\0';
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
	//TODO: handle SIGWINCH (aka resize events)
	//set the correct terminal size constraints before we go crazy and allocate windows with the wrong ones
	getmaxyx(stdscr,height,width);
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
		
		//store the previous input buffer so we know if it changed
		char old_input_buffer[BUFFER_SIZE];
		strncpy(old_input_buffer,input_buffer,BUFFER_SIZE);
		
		c=wgetch(user_input);
		if(c>=0){
			switch(c){
				//handle ctrl+c gracefully
				case BREAK:
					done=TRUE;
					break;
				//user hit enter, meaning parse and handle the user's input
				case '\n':
					//note the input_buffer gets reset to all NULL after parse_input
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
				//TODO: handle text entry history
				case KEY_UP:
					break;
				case KEY_DOWN:
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
							}
						}
					}
					//update the server list display to reflect this server no longer being there (EVEN IF this wasn't the currently selected server, which is why I can't combine this with the above loop)
					wclear(server_list);
					wmove(server_list,0,0);
					for(n=0;n<MAX_SERVERS;n++){
						if(servers[n]!=NULL){
							if(current_server==n){
								wattron(server_list,A_BOLD);
								wprintw(server_list,servers[n]->server_name);
								wattroff(server_list,A_BOLD);
							}else{
								wprintw(server_list,servers[n]->server_name);
							}
							wprintw(server_list," | ");
						}
					}
					wrefresh(server_list);
				}else if(bytes_transferred>0){
					//add this byte to the total buffer
					if(strinsert(servers[server_index]->read_buffer,one_byte_buffer[0],strlen(servers[server_index]->read_buffer),BUFFER_SIZE)){
						//a newline ends the reading and makes us start from scratch for the next byte
						if(one_byte_buffer[0]=='\n'){
							//clear out the remainder of the buffer since we re-use this memory
							int n;
							for(n=strlen(servers[server_index]->read_buffer);n<BUFFER_SIZE;n++){
								servers[server_index]->read_buffer[n]='\0';
							}
#ifdef DEBUG
							fprintf(debug_output,"main debug 0, read from server: %s",servers[server_index]->read_buffer);
#endif
							
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
								//take out the trailing newline
								substr(servers[server_index]->read_buffer,servers[server_index]->read_buffer,0,strfind("\n",servers[server_index]->read_buffer));
								
								//add the message to the relevant channel scrollback structure
								//TODO: make this parse PMs and such and not just ALWAYS go to the system channel
								char **scrollback=servers[server_index]->channel_content[0];
								
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
								strncpy(scrollback[scrollback_line],servers[server_index]->read_buffer,BUFFER_SIZE);
								
								//if this was currently in view update it there
								if(current_server==server_index){
									//number of lines of scrollback available
									//the most recently added line was the last one, but since this is 0 indexed the length is that+1
									int line_count=scrollback_line+1;
									
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
									
									//TODO: word wrap, and do it sanely
									int output_line;
									for(output_line=0;output_line<w_height;output_line++){
										//if there's text to display on this line
										if(scrollback[(line_count-w_height)+output_line]!=NULL){
											//start at the start of the line
											wmove(channel_text,output_line,0);
											wprintw(channel_text,scrollback[(line_count-w_height)+output_line]);
										}
									}
									//refresh the channel text window
									wrefresh(channel_text);
								}
							}
							
							//clear out the real buffer for the next line from the server
							for(n=0;n<BUFFER_SIZE;n++){
								servers[server_index]->read_buffer[n]='\0';
							}
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
		
		//TODO: output the most up-to-date information about servers, channels, topics, and various whatnot
		//(do this where changes occur so we're not CONSTANTLY refreshing, which causes flicker among other things)
		if(old_server!=current_server){
			//update the display of the server list
			wclear(server_list);
			wmove(server_list,0,0);
			for(n=0;n<MAX_SERVERS;n++){
				if(servers[n]!=NULL){
					if(current_server==n){
						wattron(server_list,A_BOLD);
						wprintw(server_list,servers[n]->server_name);
						wattroff(server_list,A_BOLD);
					}else{
						wprintw(server_list,servers[n]->server_name);
					}
					wprintw(server_list," | ");
				}
			}
			wrefresh(server_list);
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
/*	//free all the RAM we allocated for anything
	int server_index;
	for(server_index=0;server_index<MAX_SERVERS;server_index++){
		//if this is a valid server connection
		if(servers[server_index]!=NULL){
			safe_send(servers[server_index]->socket_fd,"QUIT :accidental_irc exited\n");
			close(servers[server_index]->socket_fd);
			
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
		}
	}
*/
	
	
#ifdef DEBUG
	fclose(debug_output);
#endif
	
	//end ncurses cleanly
	endwin();
	
	return 0;
}

