/*
 *  RDS parser
 *
 *  Copyright (C) 2020 Carsten Groß 
 * 
 *  Totally out of control:
 *  German and Swiss radio on DVB-S (and also DVB-C) supports RDS within 
 *  the MPEG transport stream. The RDS data (Radio Data System, that 
 *  thing from the Analog Radio on FM) is just inserted inside the 
 *  padding bytes of the MPEG stream. Since there are not too many 
 *  padding bytes, the RDS info elements are often spread over several MPEG frames.
 * 
 *  In the MPEG transport stream there is usually no title and 
 *  artist information in the MPEG EPG data, but it is contained in the RDS data.
 *
 *  This programm is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *  
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "ts2shout.h"

extern programm_info_t *global_state;

static struct {
	uint8_t rt[255];
	uint8_t ps[255];
	bool rt_changed;
} rds_info; 

/* This is just a debug function, not needed for normal operation */
void DumpHex(const void* data, size_t size) {
	char ascii[17];
	size_t i, j;
	ascii[16] = '\0';
	for (i = 0; i < size; ++i) {
		fprintf(stderr, "%02X ", ((unsigned char*)data)[i]);
		if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
			ascii[i % 16] = ((unsigned char*)data)[i];
		} else {
			ascii[i % 16] = '.';
		}
		if ((i+1) % 8 == 0 || i+1 == size) {
			fprintf(stderr, " ");
			if ((i+1) % 16 == 0) {
				fprintf(stderr, "|  %s \n", ascii);
			} else if (i+1 == size) {
				ascii[(i+1) % 16] = '\0';
				if ((i+1) % 16 <= 8) {
					fprintf(stderr, " ");
				}
				for (j = (i+1) % 16; j < 16; ++j) {
					fprintf(stderr, "   ");
				}
				fprintf(stderr, "|  %s \n", ascii);
			}
		}
	}
}


/* Convert EBU table 1 to latin 1
 * Taken out of http://www.interactive-radio-system.com/docs/EN50067_RDS_Standard.pdf 
 * only german at the moment, therefore no table */

uint8_t ebu2latin1(uint8_t character) {

	switch (character) {
		case 0x91: /* ä "ae" */
			return 0xe4; 
		case 0x97: /* ö "oe" */
			return 0xf6;
		case 0x99: /* ü "ue" */
			return 0xfc;
		case 0x8d: /* ß "ss" */
			return 0xdf;
		case 0xd1: /* Ä "AE" */
			return 0xc4;
		case 0xd7: /* Ö "OE" */
			return 0xd6;
		case 0xd9: /* Ü "UE" */
			return 0xdc;
	}
	if (character < 0x20 || character > 0x7f) {
		return '.';
	}
	return character; 
}

bool check_message(uint8_t* rds_message, uint8_t size) {
	/* implement crc check */
	return true;
}

void handle_rt(uint8_t* rds_message, uint8_t size) {
	uint8_t msg_len = rds_message[7]; 
	uint8_t index   = rds_message[8]; 
	if (index > 1) index = 1; 
	uint8_t i; 
	if (msg_len > 0x41) {
		msg_len = 0x41;
	}
	if (msg_len > 0) {
		for (i = msg_len - 1; i < 0x40; i++) {
			rds_info.rt[i + index * 0x40] = ' ';
		}
	}
	for (i = 9; i < 8 + msg_len; i++) {
		/* is some character different? */
		if (rds_info.rt[i - 9 + index * 0x40] != ebu2latin1(rds_message[i])) {
			rds_info.rt_changed = true; 
		}
		rds_info.rt[i - 9 + index * 0x40] = ebu2latin1(rds_message[i]); 
	}
	return; 
}
	
// Handle a RDS data chunk. 
void handle_message(uint8_t* rds_message, uint8_t size) {
	// fprintf(stderr, "  **** FULL RDS-Message (%d) ****\n", size); 
	// DumpHex(rds_message, size); 
	
	// There is a CRC 16 at the end of each message, but I don't know 
	// now to check it at the moment
	uint8_t type = rds_message[4];
	if (! check_message(rds_message, size))  
		return;
	switch (type) {
		case 0x0a: // RT (Radiotexta)
			handle_rt(rds_message, size); 
			break;
		case 0x01: // PI Code
			//handle_pi(rds_message, size); 
			break;
		case 0x02:
			//handle_ps(rds_message, size); 
			break;
	}
	if (rds_info.rt_changed == true) {
		unsigned char utf8_rt[STR_BUF_SIZE];
		unsigned char short_rt[STR_BUF_SIZE]; 
		uint8_t i = 0; 
		uint8_t j = 0;
		bool space = false;
		for (i = 0; i < strlen((char*)rds_info.rt); i++) {
			if (rds_info.rt[i] == 0x20 && space == false) {
				space = true; 
				short_rt[j] = 0x20; 
				j++;
				continue;
			} else if ( rds_info.rt[i] != 0x20 ) {
				space = false; 
				short_rt[j] = rds_info.rt[i]; 
				j++;
			}
		}
		short_rt[j] = 0;
		/* The first radiotext message disables EIT scan and 
		 * enables RDS text (if prefer_rds is enabled) */
		if (global_state->found_rds == false) {
			global_state->found_rds = true;
			output_logmessage("RDS: RDS data found, using RDS instead of EIT.\n");
		}
		/* copy RDS to stream_title */
		strcpy(global_state->stream_title, (char*)short_rt);
        utf8((unsigned char*)short_rt, utf8_rt);
		output_logmessage("RDS: %s\n", utf8_rt);
		// fprintf(stderr, "NEW RT(%s)\n", rds_info.rt);
		rds_info.rt_changed = false;
	}
	return;
}

// decode exactly one frame
// char * text points to 255 byte of char 
// buffer is the buffer of the mpeg-frame and offset is the current read offset
// it points to the "0xff" of the mpeg frame start!
char * rds_decode_oneframe(uint8_t* buffer, int offset, char *text) {
	static uint8_t current_pos = 0;
	static uint8_t rds_message[255]; 

	int j = 0;
	int k = 0; 
	uint8_t rds_data_size = buffer[offset - 2];
	if (rds_data_size == 0) {
		return NULL;
	}
	/* 0xfd, 0xfe (start marker) and 0xff (end marker) have special meanings */
	/* 0xfd is a special marker to make 0xfe and 0xff possible as data bytes */
	for (j = 3; j < ( rds_data_size + 3); j++) {
		uint8_t mychar = buffer[offset - j]; 
		if (mychar == 0xfe) {
			current_pos = 0; 
		} else if (mychar == 0xff) {
			handle_message(rds_message, current_pos); 
			current_pos = 0; 
		} else if (mychar == 0xfd) {
			// special marker: 0xfd 0x01 means 0xfe, 0xfd 0x02 means 0xff 
			j++; 
			rds_message[current_pos] = mychar + buffer[offset - j]; 
			current_pos ++; 
		} else {
			rds_message[current_pos] = mychar; 
			current_pos ++; 
		}
		text[k] = mychar; 
		k ++; 
	}
	text[k] = 0; 
	return text; 
}



/* RDS scan
   scan a whole buffer for "0xff 0xfx" - this is the beginning of an MPEG frame.
   The RDS data is right before it
   See https://www.etsi.org/deliver/etsi_ts/101100_101199/101154/02.01.01_60/ts_101154v020101p.pdf
   for a spec on how the data is aligned
 
   MPEG stream laylout looks like this (@ frame start)

   Without RDS support 

   [...] aa aa aa aa aa ff fx 
              ^^^^^^^^^ ^^^^
                  |     MPEG frame start (x is "don't care")
                  +---- padding bytes (from last frame) 
 
   No RDS data (but supported):

    [...] aa aa aa 00 fd ff fx
                   ^  ^  ^
                   |  |  +-- MPEG frame start (x is "don't care")
                   |  + RDS Marker
                   + length marker (here: 0 no data)

    RDS data available:

    [...] ff 65 2f 20 57 42 31 52 57 53 0a fd ff fx
                                     ^  ^  ^  ^
                                     |  |  |  +-- MPEG frame start
                                     |  |  + RDS Marker
                                     |  + length marker (here: 0x0a 10 Bytes of data)
                                     + Data in *reverse* order
    The data of the above block in correct order:
    53 57 52 31 42 57 20 2f 65 ff  |  SWR1BW /e.

  The sequence of the individual bytes is to be reversed. The data must be extracted from the individual
  MPEG frames and then appended to each other. The start marker is "0xfe" and the end marker is "0xff".
  The bytes inbetween 0xfe und 0xff have to be collected and stored into a buffer and handled as RDS message.
*/

void rds_data_scan(ts2shout_channel_t *chan) {

	/* RDS globally enabled? Command line option or
	 * environment variable */
	if (! global_state->prefer_rds) 
		return;
	static uint8_t oldbuffer[60];

	int i = 0;

	// Easier handling 
	uint8_t * buffer = chan->buf; 
	int size = chan->payload_size; 
	if (size < 2)
		return;
	for (i = 0; i < size ; i++) {
		if( buffer[i] == 0xff 
			&& (buffer[i+1] & 0xf0) == 0xf0 ) {
			// Found mpeg frame start. The RDS data is RIGHT IN FRONT OF IT
			if (i == 0) {
				// The frame start is too early, cannot fetch anything before
				// fprintf(stderr, "Frame start to early (%d) [0x%x 0x%x 0x%x]\n", i, oldbuffer[57 - i], oldbuffer[58 -i], oldbuffer[59 - i]);
				if (oldbuffer[59 - i] == 0xfd) {	
					char text[255];
					uint8_t rds_data_size = oldbuffer[58 - i]; 
					rds_decode_oneframe(oldbuffer, 60 - i, (char *) &text);	
					if ( rds_data_size > 0) {
						// fprintf(stderr, "RDS-Frame in oldbuf (%d)\n", rds_data_size);
						// DumpHex(text, rds_data_size);
					}
				}
			} else { // if ( (i - offset) > chan->mpah.framesize - 30 
				if ( buffer[i-1] == 0xfd ) {	
					// yes: RDS data
					uint8_t rds_data_size = buffer[i-2]; 
					if (rds_data_size > 0) {
						char text[255];
						rds_decode_oneframe(buffer, i, (char *) &text); 
						if ( rds_data_size > 0 ) {
							// fprintf(stderr, "RDS-Frame (0x%x, %d)\n", i, rds_data_size);
							// DumpHex(text, rds_data_size); 
						} 
					} else {
						// fprintf(stderr, "No RDS Data (0x%x)\n", i); 
					}
				} else {
					// fprintf(stderr, "No 0xfd? Data (0x%x)\n", i); 
				}
			}
			// offset = i;			// current offset 
			// matchcount += 1;    // current match
		}
	}
	/* Store last 60 bytes from the end of the frame into "oldbuffer" 
	 * to have it at hand if next frame starts with MPEG header */
	memcpy(oldbuffer, buffer + size - 60, 60); 
	// fprintf(stderr, "Offset %d, matchcount %d\n", offset, matchcount); 
	return;  
}

void init_rds() {
	memset(rds_info.rt, 0x20, 0x7f); 
	return; 
}
