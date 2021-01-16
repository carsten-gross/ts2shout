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
 * non latin1 characters are translated to "." */

static uint8_t ebutable1[] = {
	/* 0    1    2    3    4    5    6    7    8    9    a    b    c    d    e    f     */
	0xe1,0xe0,0xe9,0xe8,0xed,0xec,0xf3,0xf2,0xfa,0xf9,0xd1,0xc7,0x2e,0xdf,0xa1,0x2e,	/* 0x80 - 0x8f */
	0xe2,0xe4,0xea,0xeb,0xee,0xef,0xf4,0xf6,0xfb,0xfc,0xf1,0xe7,0x2e,0x2e,0x2e,0x2e,	/* 0x90 - 0x9f */
	0xaa,0x2e,0xa9,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0x24,0x2e,0x2e,0x2e,0x2e,	/* 0xa0 - 0xaf */
	0xba,0xb9,0xb2,0xb3,0xb1,0x2e,0x2e,0x2e,0xb5,0xbf,0xf7,0xb0,0xbc,0xbd,0xbe,0xa7,	/* 0xb0 - 0xbf */
	0xc1,0xc0,0xc9,0xc8,0xcd,0xcc,0xd3,0xd2,0xda,0xd9,0x2e,0x2e,0x2e,0x2e,0xd0,0x2d,	/* 0xc0 - 0xcf */
	0xc2,0xc4,0xca,0xcb,0xce,0xcf,0xd4,0xd6,0xdb,0xdc,0x2d,0x2e,0x2e,0x2e,0x2e,0x2e,	/* 0xd0 - 0xdf */
	0xc3,0xc5,0xc6,0x2e,0x2e,0xdd,0xd5,0xd8,0xde,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0xf0,	/* 0xe0 - 0xef */
	0xe3,0xe5,0xe6,0x2e,0x2e,0xfd,0xf5,0xf8,0xfe,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,	/* 0xf0 - 0xff */
};

static uint8_t ebu2latin1(uint8_t character) {
	if (character < 0x20) {
		if (character == 0x0d || character == 0x0a) {
			/* NDR2 uses this. It's documented as proposed linefeed or CR. 
			   We replace it with space because we use a single line display */
			return ' '; 
		}
		return '.';
	}
	if (character >= 0x80) {
		return ebutable1[(character & 0x7f)];
	}
	return character;
}

bool check_message(uint8_t* rds_message, uint8_t size) {
	/* Calculate the CRC16 of the RDS frame to get rid of wrong frames */
	uint16_t crc_result = crc16(rds_message, size);
	if (crc_result != 0) {
#ifdef DEBUG
		output_logmessage("CRC16 error in RDS Message (size=%d, crc_result=0x%x)\n", size, crc_result);
		DumpHex(rds_message, size);
#endif
		return false;
	}
	return true;
}

void handle_rt(uint8_t* rds_message, uint8_t size) {
	uint8_t msg_len = rds_message[7]; 
	uint8_t index   = rds_message[8]; 
	/* Radiotext consists of two message parts with 64 characters each
	 * indexed by an index being either 0 or 1 */
	if (index > 1) index = 1; 
	uint8_t i; 
	if (msg_len > 0x41) {
		msg_len = 0x41;
	}
	/* Cleanup old message */
	if (msg_len > 0) {
		for (i = msg_len - 1; i < 0x40; i++) {
			rds_info.rt[i + index * 0x40] = ' ';
		}
	}
	/* Check and convert message to latin1 */
	for (i = 9; i < 8 + msg_len; i++) {
		/* is some character different? */
#ifdef DEBUG
		if (( rds_message[i] < 0x20 || rds_message[i] > 0x7f) && ebu2latin1(rds_message[i]) == 0x2e) {
			fprintf(stderr, "RDS: SORRY could not convert character code 0x%x into latin1.\n", rds_message[i]); 
		}
#endif 
		if (rds_info.rt[i - 9 + index * 0x40] != ebu2latin1(rds_message[i])) {
			rds_info.rt_changed = true; 
		}
		rds_info.rt[i - 9 + index * 0x40] = ebu2latin1(rds_message[i]); 
	}
	/* Shorten message
	 * Check whether the "first" RT message is the same as the "second"
	 * in this case delete the "second" and try to rearrange the first (see below) */
	if (memcmp(rds_info.rt, rds_info.rt + 0x40, 0x40) == 0) {
		/* Message is exactly the same */
		bool exchange = 0;
		char text1[64];
		char text2[64];
		uint8_t size1 = 0;
		uint8_t size2 = 0;
		uint8_t string_size = 0;
#ifdef DEBUG
		fprintf(stderr, "RDS: Message shorting is going on %.64s == %.64s\n", rds_info.rt, rds_info.rt + 0x40);
#endif
		memset(rds_info.rt + 0x40, ' ', 0x40);
		/* SWR1 and perhaps other stations send out "title / interpret"
		 * we want "interpret - title" to get the correct arrangement on the
		 * Squeezebox players. This improves the display.
		 * It is only called it first message equals the second one */

		/* Search end of string */
		for (i = 1; i < (msg_len - 1); i++) {
			if (rds_info.rt[i] != ' ') string_size = i;
		}
		/* exchange "text1 / text2" is converted to "text2 - text1" */
		/* This is here for SWR1, 2 and 3 */
		for (i = 1; i < (string_size - 1); i++) {
			if (! exchange) {
				/* bail out if there is already a `-' inside the text */
				if (rds_info.rt[i] == '-') {
					exchange = 1;
					continue;
				}
				if (rds_info.rt[i - 1] == ' ' && rds_info.rt[i] == '/' && rds_info.rt[i + 1] == ' ' ) {
					/* Found text1 / text2 ? */
					memcpy(text1, rds_info.rt, i - 1);
					size1 = i - 1;
					size2 = string_size - i - 1;
					memcpy(text2, rds_info.rt + i + 2, size2);
					/* text2 - text1    this is the new order */
					memcpy(rds_info.rt, text2, size2);
					rds_info.rt[size2] = ' ';
					rds_info.rt[size2 + 1] = '-';
					rds_info.rt[size2 + 2] = ' ';
					memcpy(rds_info.rt + size2 + 3, text1, size1);
					exchange = 1;
				}
			}
		}
		/* exchange "text1 von text2" -> "text2 - text1" */
		/* This is needed for SRF1~3 - only executed if above function doesn't find something */
		for (i = 1; i < (string_size - 3); i++) {
			if (! exchange) {
				/* bail out if there is already a `-' inside the text */
				if (rds_info.rt[i] == '-') {
					exchange = 1;
					continue;
				}
				if (rds_info.rt[i - 1] == ' '
					&& rds_info.rt[i + 0] == 'v'
					&& rds_info.rt[i + 1] == 'o'
					&& rds_info.rt[i + 2] == 'n'
					&& rds_info.rt[i + 3] == ' ' ) {
					/* Found text1 / text2 ? */
					memcpy(text1, rds_info.rt, i - 1);
					size1 = i - 1;
					size2 = string_size - i - 3;
					memcpy(text2, rds_info.rt + i + 4, size2);
					/* text2 - text1    this is the new order */
					memcpy(rds_info.rt, text2, size2);
					rds_info.rt[size2] = ' ';
					rds_info.rt[size2 + 1] = '-';
					rds_info.rt[size2 + 2] = ' ';
					memcpy(rds_info.rt + size2 + 3, text1, size1);
					memcpy(rds_info.rt + string_size - 1, "     ", 3);
					exchange = 1;
				}
			}
		}
	}
	return; 
}
	
/* Handle a RDS data chunk. */
void handle_message(uint8_t* rds_message, uint8_t size) {
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

/* decode exactly one frame -  char * text points to 255 byte of char
 * buffer is the buffer of the mpeg-frame and offset is the current read offset
 * it points to the "0xff" of the mpeg frame start! */
void rds_decode_oneframe(uint8_t* buffer, int offset) {
	static uint8_t current_pos = 0;
	static uint8_t rds_message[255]; 

	int j = 0;
	int k = 0; 
	uint8_t rds_data_size = buffer[offset - 2];
	if (rds_data_size == 0) {
		return;
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
		k ++; 
	}
	return;
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
	if (size < 60) {
		return;
	}
	for (i = 0; i < size - 3; i++) {
		if( ( buffer[i] == chan->mpah.sync0 )
			&& (buffer[i+1] == chan->mpah.sync1 )
			&& (buffer[i+2] == chan->mpah.sync2 )
			&& (buffer[i+3] == chan->mpah.sync3 ) ) {
			/* Sometimes we find ff fx data somewhere inside the frame ... this happens not very often, but
			   could corrupt the display of a RDS message. As we calculate the crc16 the use of a corrupted
			   message is unlikely (but not impossible).
			   Correct solution would be to count the bytes and align it with frame sizes, but it's
			   hard to keep this correct if MPEG frame sync is lost in main reception loop. Therefore
			   we just ignore this problem. In very rare cases the mpeg frame border is directly on the
			   buffer border. Some RDS message will be lost then. This can happen if you've continuity errors. */
			/* Found mpeg frame start. The RDS data is RIGHT IN FRONT OF IT */
			if ( i < 32 ) {
				/* If we find the marker at the very beginning of the buffer
				 * we can expect that the current buffer does not contain all necessary data.
				 * In this case we also use data from last frame. */
				uint8_t helper[512];       /* with this size we cannot ever read outside our memory */
				memset(helper, 0xff, 512); /* write termination character all over the buffer */
				memcpy(helper + 255 , oldbuffer + i, 60 - i); /* Fill in old buffer contents from last call */
				memcpy(helper + 255 + 60 - i, buffer, i);     /* and append the new buffer contents */
				if (helper[255 + 59 - i] == 0xfd) {
					uint8_t rds_data_size = helper[255 + 58 -i];
					if (rds_data_size > 0) {
						rds_decode_oneframe(helper, 255 + 60 - i);
					}
				} else {
				}
			} else { /* if (i < 32) */
				if ( buffer[i-1] == 0xfd ) {	
					/* yes: RDS data */
					uint8_t rds_data_size = buffer[i-2]; 
					if (rds_data_size > 0) {
						rds_decode_oneframe(buffer, i);
						if ( rds_data_size > 0 ) {
							// fprintf(stderr, "RDS-Frame (0x%x, %d)\n", i, rds_data_size);
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
	 * to have it at hand if we find a MPEG header near the start
	 * of the next frame. */
	memcpy(oldbuffer, buffer + size - 60, 60); 
	return;  
}

void init_rds() {
	memset(rds_info.rt, ' ', 0x80);
	return;
}
