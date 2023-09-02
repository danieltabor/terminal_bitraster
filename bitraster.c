/*
 * Copyright (c) 2023, Daniel Tabor
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <stdio.h>

static int reverse_byte = 0;
static int fd = -1;
static off_t offset = 0;
static off_t fd_size = 0;
static uint8_t* buffer = 0;
static size_t buffer_size = 0;
static off_t buffer_offset = -1;
static size_t buffer_width = 0;
static int last_term_w = 0;
static int last_term_h = 0;
static int col_offset = 0;
static int delay_ms = 250;
static int life = 0;
static uint8_t* life_buffer = 0;

#define UTF8_IMPLEMENTATION
#include "utf8.h"

#define TTY struct termios
#define GET_TTY(fd, buf) tcgetattr(fd, buf)
#define SET_TTY(fd, buf) tcsetattr(fd, TCSADRAIN, buf)

#define DIRUP 0x41
#define DIRDN 0x42
#define DIRRT 0x43
#define DIRLT 0x44

#define ERROR(...) { term_reset(); fprintf(stderr,__VA_ARGS__); exit(-1); }
#define TERM_ERROR(...) { fprintf(stderr,__VA_ARGS__); exit(-1); }

static void usage(char* cmd) {
	char* cmd_filename = cmd+strlen(cmd);
	while( cmd_filename > cmd ) {
		if( *(cmd_filename-1) != '/' ) {
			cmd_filename--;
		} else {
			break;
		}
	}
	fprintf(stderr,"Usage:\n");
	fprintf(stderr,"%s [-h] [-r] [-wWidth] [-oOffset] [-dDelayMS] [path]\n",cmd_filename);
	fprintf(stderr,"\n");
	fprintf(stderr,"  -w : Bit width of buffer (controls horizontal scroll)\n");
	fprintf(stderr,"       Width must be a multple of 8 bits.\n");
	fprintf(stderr,"  -o : Initial Byte offset into file\n");
	fprintf(stderr,"  -d : Delay, in millisecons, or any automatic updates\n");
	fprintf(stderr,"\n");
	fprintf(stderr,"If path is not provided, data is streamed from stdin, -w and -o are ignored\n");
	exit(0);
}

static void term_setup() {
	int flags;
	TTY tty;
	
	//Use fcntl to make stdin NON-BLOCKING
	errno = 0;
	if( (flags = fcntl(STDIN_FILENO, F_GETFL, 0)) < 0 ) {
		TERM_ERROR("Error getting STDIN flags: %s\n",strerror(errno));
	}
	flags |= O_NONBLOCK;
	errno = 0;
	if( fcntl(STDIN_FILENO, F_SETFL, flags ) < 0 ) { 
		TERM_ERROR("Error setting STDIN flags: %s\n",strerror(errno));
	}
	
	//Use ioctl/TCGETS to disable echo and line buffering (icannon)
	errno = 0;
	if( GET_TTY(STDIN_FILENO,&tty) < 0 ) {
		TERM_ERROR("Error getting termial flags: %s\n",strerror(errno));
	}
	tty.c_lflag &= ~ICANON;
	tty.c_lflag &= ~ECHO;
	errno = 0;
	if( SET_TTY(STDIN_FILENO,&tty) < 0 ) {
		TERM_ERROR("Error setting terminal flags: %s\n",strerror(errno));
	}
}

static void term_reset() {
	int flags;
	TTY tty;
	
	printf("\x1b[2J\x1b[0;0H\x1b[0m");
	
	//Use fcntl to make stdin BLOCKING
	errno = 0;
	if( (flags = fcntl(STDIN_FILENO, F_GETFL, 0)) < 0 ) {
		TERM_ERROR("Error getting STDIN flags: %s\n",strerror(errno));
	}
	flags &= ~O_NONBLOCK;
	errno = 0;
	if( fcntl(STDIN_FILENO, F_SETFL, flags ) < 0 ) { 
		TERM_ERROR("Error setting STDIN flags: %s\n",strerror(errno));
	}
	
	//Use ioctl/TCGETS to enable echo and line buffering (icannon)
	errno = 0;
	if( GET_TTY(STDIN_FILENO,&tty) < 0 ) {
		TERM_ERROR("Error getting termial flags: %s\n",strerror(errno));
	}
	tty.c_lflag |= ICANON;
	tty.c_lflag |= ECHO;
	errno = 0;
	if( SET_TTY(STDIN_FILENO,&tty) < 0 ) {
		TERM_ERROR("Error setting terminal flags: %s\n",strerror(errno));
	}
	
	fflush(stdout);
}

static void term_size(int* width, int* height) {
	struct winsize ws;
	
	//Use ioctl/TIOCGWINSZ to get terminal size
	errno = 0;
	if( ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws) < 0 ) {
		ERROR("Error geting terminal size: %s\n",strerror(errno));
	}
	*width = ws.ws_col;
	*height = ws.ws_row;
}

static inline int getbit(uint8_t* buf, int x, int y) {
	size_t bit_index; 
	size_t byte_index;
	uint8_t byte_shift;
	
	if( x < 0 || x >= buffer_width || y < 0 ) {
		return 0;
	}
	
	bit_index = y*buffer_width + x;
	byte_index = bit_index/8;
	if( byte_index >= buffer_size ) {
		return 0;
	}
	if( reverse_byte ) {
		byte_shift = bit_index%8;
	}
	else {
		byte_shift = 7-(bit_index%8);
	}
	return (buf[byte_index]>>byte_shift) & 1;
}

static inline void setbit(uint8_t* buf, int x, int y) {
	size_t bit_index; 
	size_t byte_index;
	uint8_t byte_shift;
	
	if( x < 0 || x > buffer_width || y < 0 ) {
		return;
	}
	
	bit_index = y*buffer_width+x;
	byte_index = bit_index/8;
	if( byte_index > buffer_size ) {
		return;
	}
	if( reverse_byte ) {
		byte_shift = bit_index%8;
	}
	else {
		byte_shift = 7-(bit_index%8);
	}
	buf[byte_index] |= (1<<byte_shift);
}

static uint32_t sextant_chars[64] = {
	0x00020,0x1FB1E,0x1FB0F,0x1FB2D,0x1FB07,0x1FB26,0x1FB16,0x1FB35,
	0x1FB03,0x1FB22,0x1FB13,0x1FB31,0x1FB0B,0x1FB29,0x1FB1A,0x1FB39,
	0x1FB01,0x1FB20,0x1FB11,0x1FB2F,0x1FB09,0x02590,0x1FB18,0x1FB37,
	0x1FB05,0x1FB24,0x1FB14,0x1FB33,0x1FB0D,0x1FB2B,0x1FB1C,0x1FB3B,
	0x1FB00,0x1FB1F,0x1FB10,0x1FB2E,0x1FB08,0x1FB27,0x1FB17,0x1FB36,
	0x1FB04,0x1FB23,0x0258C,0x1FB32,0x1FB0C,0x1FB2A,0x1FB1B,0x1FB3A,
	0x1FB02,0x1FB21,0x1FB12,0x1FB30,0x1FB0A,0x1FB28,0x1FB19,0x1FB38,
	0x1FB06,0x1FB25,0x1FB15,0x1FB34,0x1FB0E,0x1FB2C,0x1FB1D,0x02588
};

static void update() {
	int term_w, term_h;
	int char_x, char_y;
	int disp_w;
	int off_x;
	size_t new_buffer_size;
	uint8_t* tmp;
	uint8_t index;
	
	term_size(&term_w,&term_h);
	if(   term_h != last_term_h || 
	      term_w != last_term_w || 
	      buffer_offset != offset ) {
		//If left unset, set buffer_width the maximum displayable
		//number of bits
		if( !buffer_width ) {
			buffer_width = term_w*2;
		}
		if( buffer_width % 8 ) {
			buffer_width = buffer_width - (buffer_width % 8);
		}
		
		//Determine (based on current terminal size)
		//how many Bytes of data can be displayed and resize
		//buffer to accept them
		new_buffer_size = (term_h*3) * buffer_width;
		if( new_buffer_size % 8 ) {
			new_buffer_size = new_buffer_size/8+1;
		}
		else {
			new_buffer_size = new_buffer_size/8;
		}
		if( new_buffer_size > fd_size ) {
			new_buffer_size = fd_size;
		}
		if( new_buffer_size != buffer_size ) {
			errno = 0;
			tmp = realloc(buffer,new_buffer_size);
			if( !tmp ) {
				free(buffer);
				ERROR("Memory allocation error: %s\n",strerror(errno));
			}
			buffer = tmp;
			buffer_size = new_buffer_size;
		}
		
		//Seek and read the file
		if( offset + buffer_size > fd_size ) {
			offset = fd_size - buffer_size;
		}
		if( offset < 0 ) {
			offset = 0;
		}
		errno = 0;
		if( lseek(fd,offset,SEEK_SET) < 0 ) {
			ERROR("File seek error: %s",strerror(errno));
		}
		errno = 0;
		if( read(fd,buffer,buffer_size) != (ssize_t)buffer_size ) {
			ERROR("File read error: %s\n",strerror(errno));
		}

		last_term_h = term_h;
		last_term_w = term_w;
		buffer_offset = offset;
	}
	
	if( col_offset + term_w*2 > buffer_width ) {
		col_offset = buffer_width - term_w*2;
	}
	if( col_offset < 0 ) {
		col_offset = 0;
	}
	
	disp_w = buffer_width/2;
	if( disp_w > term_w ) {
		disp_w = term_w;
	}
	
	printf("\x1b[2J\x1b[H\x1b[0m");
	for( char_y=0; char_y<term_h; char_y++ ) {
		if( char_y ) {
			printf("\n");
		}
		for( char_x=0; char_x<disp_w; char_x++ ) {
			off_x = col_offset + char_x*2;
			index = 0;
			index = (index<<1) | getbit(buffer,off_x  , char_y*3   );
			index = (index<<1) | getbit(buffer,off_x+1, char_y*3   );
			index = (index<<1) | getbit(buffer,off_x  ,(char_y*3)+1);
			index = (index<<1) | getbit(buffer,off_x+1,(char_y*3)+1);
			index = (index<<1) | getbit(buffer,off_x  ,(char_y*3)+2);
			index = (index<<1) | getbit(buffer,off_x+1,(char_y*3)+2);
			printf("%s",utf8_encode(0,sextant_chars[index]));
		}
	}
	fflush(stdout);
}

static void step_life() {
	int count;
	int x,y;
	int h = (buffer_size*8)/buffer_width;
	if( !life_buffer ) {
		life_buffer = malloc(buffer_size);
	}
	memset(life_buffer,0,buffer_size);
	for( y=0; y<h; y++ ) {
		for( x=0; x<buffer_width; x++ ) {
			count = 0;
			count += getbit(buffer,x-1,y-1);
			count += getbit(buffer,x  ,y-1);
			count += getbit(buffer,x+1,y-1);
			count += getbit(buffer,x-1,y  );
			count += getbit(buffer,x+1,y  );
			count += getbit(buffer,x-1,y+1);
			count += getbit(buffer,x  ,y+1);
			count += getbit(buffer,x+1,y+1);
			if( getbit(buffer,x,y) ) {
				if( count == 2 || count == 3 ) {
					setbit(life_buffer,x,y);
				}
			}
			else {
				if( count == 3 ) {
					setbit(life_buffer,x,y);
				}
			}
		}
	}
	memcpy(buffer,life_buffer,buffer_size);
}

void run_sigint_handler(int signalId) {
	(void)signalId;
	term_reset();
	exit(0);
}

static void run() {
	uint8_t input[8];
	ssize_t inputlen;
	struct sigaction action;
	
	action.sa_handler = run_sigint_handler;
	sigaction(SIGINT, &action, 0);
	
	term_setup();
	update();
	
	for(;;) {
		memset(input,0,sizeof(input));
		inputlen = read(STDIN_FILENO,&input,sizeof(input));
		if( inputlen < 0 ) {
			if( errno != EAGAIN ) {
				break;
			}
			if( life ) {
				step_life();
				update();
				usleep(delay_ms*1000);
			}
			else {
				usleep(100000);
			}
			continue;
		}
		//Regular Input
		else if( inputlen == 1 ) {
			if( input[0] == 0x1b ) {
				break;
			}
			if( input[0] == 'q' || input[0] == 'Q' ) {
				break;
			}
			else if( input[0] == 'i' || input[0] == 'I' ) {
				printf("\rFile Offset: 0x%08lx  Bit Offset: 0x%08x",offset,col_offset);
				fflush(stdout);
				continue;
			}
			else if( input[0] == 'h' || input[0] == 'H' ) {
				col_offset--;
			}
			else if( input[0] == 'j' || input[0] == 'J' ) {
				offset = offset + buffer_width/8;
			}
			else if( input[0] == 'k' || input[0] == 'K' ) {
				offset = offset - buffer_width/8;
			}
			else if( input[0] == 'l' || input[0] == 'L' ) {
				col_offset++;
			}
			else if( input[0] == 'r' || input[0] == 'R' ) {
				life = 1;
				continue;
			}
		}
		else if( inputlen == 3 ) {
			if( input[0] == 0x1B && (input[1] == 0x5B || input[1] == 0x4F) ) {
				if( input[2] == DIRUP ) { //Arrow Up
					offset = offset - buffer_width/8;
				}
				else if( input[2] == DIRDN ) { //Arrow Down
					offset = offset + buffer_width/8;
				}
				else if( input[2] == DIRRT ) { //Arrow Right
					col_offset++;
				}
				else if( input[2] == DIRLT ) { //Arrow Left
					col_offset--;
				}
				else if( input[2] == 0x46 ) { //End
					offset = fd_size;
				}
				else if( input[2] == 0x48 ) { //Home
					offset = 0;
				}
			}
		}
		else if( inputlen == 4 ) {
			if( input[0] == 0x1B && input[1] == 0x5B && input[3] == 0x7E ) {
				if( input[2] == 0x35 ) { //Page Up
					offset = offset - buffer_size;
				}
				else if( input[2] == 0x36 ) { //Page Down
					offset = offset + buffer_size;
				}
			}
		}
		if( life ) {
			life = 0;
			free(life_buffer);
			life_buffer = 0;
			buffer_offset = -1;
		}
		update();
	}
	
	term_reset();
}

void stream_sigint_handler(int signalId) {
	(void)signalId;
	exit(0);
}

static void stream() {
	int term_w, term_h;
	int char_x, disp_w;
	uint8_t* tmp;
	uint8_t index;
	struct sigaction action;
	
	action.sa_handler = stream_sigint_handler;
	sigaction(SIGINT, &action, 0);
	
	for(;;) {
		term_size(&term_w,&term_h);
		if( !buffer_width ) {
			buffer_width = term_w*2;
		}
		if( buffer_width % 8 ) {
			buffer_width = buffer_width - (buffer_width % 8);
		}
		
		buffer_size = buffer_width/8*3;
		tmp = realloc(buffer,buffer_size);
		if( !tmp ) {
			free(buffer);
			fprintf(stderr,"Memory allocation error: %s\n",strerror(errno));
			exit(-1);
		}
		buffer = tmp;
	
		if( read(STDIN_FILENO,buffer,buffer_size) != (ssize_t)buffer_size ) {
			return;
		}
		disp_w = buffer_width/2;
		for( char_x=0; char_x<disp_w; char_x++ ) {
			index = 0;
			index = (index<<1) | getbit(buffer,2*char_x  ,0);
			index = (index<<1) | getbit(buffer,2*char_x+1,0);
			index = (index<<1) | getbit(buffer,2*char_x  ,1);
			index = (index<<1) | getbit(buffer,2*char_x+1,1);
			index = (index<<1) | getbit(buffer,2*char_x  ,2);
			index = (index<<1) | getbit(buffer,2*char_x+1,2);
			printf("%s",utf8_encode(0,sextant_chars[index]));
		}
		printf("\n");
		fflush(stdout);
		
		usleep(delay_ms*1000);
	}
}

int main(int argc, char** argv) {
	int i;
	
	i = 1;
	while( i < argc ) {
		if( !strcmp(argv[i],"-h") ) {
			usage(argv[0]);
		}
		else if( !strcmp(argv[i],"-r") ) {
			reverse_byte = 1;
		}
		else if( !strncmp(argv[i],"-w",2) ) {
			errno = 0;
			buffer_width = strtoul(argv[i]+2,0,0);
			if( errno ) {
				fprintf(stderr,"Width error: %s\n\n",strerror(errno));
				usage(argv[0]);
			}
			if( buffer_width % 8 ) {
				fprintf(stderr,"Width is not even multiple of 8\n\n");
				usage(argv[0]);
			}
		}
		else if( !strncmp(argv[i],"-o",2) ) {
			errno = 0;
			offset = strtoul(argv[i]+2,0,0);
			if( errno ) {
				fprintf(stderr,"Offset error: %s\n\n",strerror(errno));
				usage(argv[0]);
			}
			else if( offset < 0 ) {
				fprintf(stderr,"Offset negative\n\n");
				usage(argv[0]);
			}
		}
		else if( !strncmp(argv[i],"-d",2) ) {
			errno = 0;
			delay_ms = strtoul(argv[i]+2,0,0);
			if( errno ) {
				fprintf(stderr,"Delay error: %s\n\n",strerror(errno));
				usage(argv[0]);
			}
		}
		else if( fd < 0 ) {
			errno = 0;
			fd = open(argv[i],O_RDONLY);
			if( fd < 0 ) {
				fprintf(stderr,"Path error: %s\n\n",strerror(errno));
				usage(argv[0]);
			}
			fd_size = lseek(fd,0,SEEK_END);
			if( fd_size < 0 ) {
				fprintf(stderr,"File size error: %s\n\n",strerror(errno));
				usage(argv[0]);
			}
		}
		else {
			usage(argv[0]);
		}
		i++;
	}
	
	if( fd < 0 ) {
		stream();
	}
	else {
		run();
	}
	
	return 0;
}
