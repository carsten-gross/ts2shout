/* 

	util.c
	(C) Carsten Gross <carsten@siski.de> 2018, 2019
	
	Copyright notice:
	
	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
    
*/

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#include "ts2shout.h"

static const char *channel_type_name[] = {
    FOREACH_CHANNEL_TYPE(GENERATE_STRING)
};

/* initialize the structure after allocating memory for it */
void init_channel (enum_channel_type channel_type, int pid, int current_channel) {
	ts2shout_channel_t *chan =  channels[ current_channel ];
	chan->channel_type = channel_type;
	chan->pid = pid; 
    chan->num = current_channel;
	chan->continuity_count = -1;
	channel_map[ pid ] = chan;
	return;
}

/* Add a channel */
int add_channel ( enum_channel_type channel_type, int pid) {
	ts2shout_channel_t *chan = NULL;
	/* Avoid logging the default */
	if ( (pid != 17) && (pid != 18) && (pid != 0) ) {	
		output_logmessage("add_channel(): Subscribing to MPEG-TS PID %d (Type %s)\n", pid, channel_name(channel_type)); 
	}
	if ( channel_count >= MAX_CHANNEL_COUNT ) {
		fprintf(stderr, "add_channel(): Trying to add more then %d channels\n", MAX_CHANNEL_COUNT); 
		return 0;
	}
	if ( channel_map[ pid ] ) {
		fprintf(stderr, "add_channel(): Channel with PID %d already exists\n", pid);
		return 0;
	}

	chan = calloc( 1, sizeof(ts2shout_channel_t) );
	if (!chan) {
		fprintf(stderr, "add_channel(): Failed to allocate memory for new channel with PID %d and channel_type %d", pid, channel_type); 
		return 0; 
	}
    channels[ channel_count ] = chan;
	init_channel(channel_type, pid, channel_count); 
	channel_count++;
	return 1; 
}

void init_structures() {
	output_logmessage("init_structures(): Subscribing to MPEG-TS PID 0, 17, 18 (%s, %s, %s)\n", 
			channel_name(CHANNEL_TYPE_PAT), channel_name(CHANNEL_TYPE_SDT), channel_name(CHANNEL_TYPE_EIT));
	if (! add_channel(CHANNEL_TYPE_PAT, 0)) 
		exit(1);
	if (! add_channel(CHANNEL_TYPE_SDT, 17)) 
		exit(1);
	if (! add_channel(CHANNEL_TYPE_EIT, 18))
		exit(1);
	return;
}

/* Get Name of channel name for better debugging */
const char* channel_name(enum_channel_type channel_type) {
	return channel_type_name[channel_type]; 
}

/* from https://stackoverflow.com/questions/4059775/convert-iso-8859-1-strings-to-utf-8-in-c-c */
unsigned char *utf8(unsigned char *in, unsigned char *out) {
	uint32_t counter = 0;
	unsigned char *outstart = out; 
	while (*in && counter+2 < STR_BUF_SIZE) {
	    if (*in<128) {
			*out++=*in++;
			counter++; 
		} else {
			*out++=0xc2+(*in>0xbf), *out++=(*in++&0x3f)+0x80;
			counter += 2;
		}
	}
	*out = 0;
	return outstart; 
}
	
	
