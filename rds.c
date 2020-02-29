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
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *  
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts2shout.h"

#undef RDS_DEBUG   /* Currently RDS is not activated, because implementation is not finished */

extern programm_info_t *global_state;

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

// Handle a RDS data chunk. Fill the data right into 
// correct fields 

void handle_rds_data(ts2shout_channel_t *chan) {

	return;
}

// decode exactly one frame
// char * text points to 255 byte of char 
// buffer is the buffer of the mpeg-frame and offset is the current read offset
// it points to the "0xff" of the mpeg frame start!
char * rds_decode_oneframe(uint8_t* buffer, int offset, char *text) {
	int j = 0;
	int k = 0; 
	uint8_t rds_data_size = buffer[offset - 2];
	if (rds_data_size == 0) {
		return NULL;
	}
	for (j = 3; j < ( rds_data_size + 3); j++) {
		uint8_t mychar = buffer[offset - j]; 
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
 
   MPEG stream laylout looks like this 
   
   No RDS data (but supported): 

	[...] aa aa aa 00 fd ff fx
                   ^  ^  ^
                   |  |  +-- MPEG frame start (x is "don't care")
                   |  + RDS Marker 
                   + lenght marker (here: 0 no data)

    RDS data available: 

	[...] ff 65 2f 20 57 42 31 52 57 53 0a fd ff fx
                                     ^  ^  ^  ^
                                     |  |  |  +-- MPEG frame start 
                                     |  |  + RDS Marker 
                                     |  + lenght marker (here: 0 no data)
                                     + Data in *reverse* order 
    The data of the above block in correct order: 
    53 57 52 31 42 57 20 2f 65 ff  |  SWR1BW /e. 

  The sequence of the individual bytes is to be reversed. The data must be extracted from the individual 
  MPEG frames and then appended to each other. The start marker is "0xfe" and the end marker is "0xff". 
  The bytes inbetween 0xfe und 0xff have to be collected and stored into a buffer and handled as RDS message.
  (Not implemented yet)
*/

#undef RDS_DEBUG
void rds_data_scan(ts2shout_channel_t *chan) {

#ifdef RDS_DEBUG
	static uint8_t oldbuffer[60];

	int i = 0;  // Loop variables
	// int matchcount = 0;

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
				fprintf(stderr, "Frame start to early (%d) [0x%x 0x%x 0x%x]\n", i, oldbuffer[57 - i], oldbuffer[58 -i], oldbuffer[59 - i]);
				if (oldbuffer[59 - i] == 0xfd) {	
					char text[255];
					uint8_t rds_data_size = oldbuffer[58 - i]; 
					rds_decode_oneframe(oldbuffer, 60 - i, (char *) &text);	
					if ( rds_data_size > 0) {
						fprintf(stderr, "RDS-Frame in oldbuf (%d)\n", rds_data_size);
						DumpHex(text, rds_data_size);
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
							fprintf(stderr, "RDS-Frame (0x%x, %d)\n", i, rds_data_size);
							DumpHex(text, rds_data_size); 
						} 
					} else {
						// fprintf(stderr, "No RDS Data (0x%x)\n", i); 
					}
				} else {
					fprintf(stderr, "No 0xfd? Data (0x%x)\n", i); 
				}
			}
			// offset = i;			// current offset 
			// matchcount += 1;    // current match
		}
	}
	// check wether there is something at the end
	/* 
	if (buffer[size-1] == 0xfd && buffer[size] == 0xff) {
		char text[255];
		rds_decode_oneframe(buffer, i, (char *) &text);
		if ( buffer[size -2] > 0 && text[0] > 0 ) {	
			// fprintf(stderr, "%s", text);
			fprintf(stderr, "RDS Frame (0x%x, %d)\n", i, buffer[size -2]);
			DumpHex(text, buffer[size -2]);
		}
	}
	*/
	/* Store last 60 bytes from the end of the frame into "oldbuffer" 
	 * to have it at hand if next frame starts with MPEG header */
	memcpy(oldbuffer, buffer + size - 60, 60); 
	// fprintf(stderr, "Offset %d, matchcount %d\n", offset, matchcount); 
#endif
	return;  
}

