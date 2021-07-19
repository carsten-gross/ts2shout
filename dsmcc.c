/*

	dsmcc.c
	(C) Carsten Gross <carsten@siski.de> 2021-
	
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>


#include "ts2shout.h"
#include "rds.h"
#include "dsmcc.h"



#define CACHE_DIRECTORY "/var/tmp/cache"

/* Automatic variable, this way initialized with 0 */

module_buffer_t * module_buffer[65535];

void handle_download_data_block(unsigned char *buf, size_t len) {
	uint16_t module_nr;
	uint16_t block_nr;
	if (DSMCC_MESSAGE_TYPE(buf) != 0x3c) {
		output_logmessage("handle_download_data_block(): internal error, called with wrong message type 0x%x\n", DSMCC_MESSAGE_TYPE(buf));
		return;
	}
	module_nr = DSMCC_MODULE_ID(buf);
	block_nr  = DSMCC_BLOCKNR(buf);
	if (module_buffer[module_nr] == NULL) {
		module_buffer[module_nr] = calloc(1,sizeof(module_buffer));
	}
	if (block_nr > 255) {
		output_logmessage("handle_download_data_block(): internal error, block_nr (%d) > 255, cannot handle\n", block_nr);
		return;
	}
	module_buffer[module_nr]->module_number = module_nr;
	module_buffer[module_nr]->block_size[block_nr] = DSMCC_MESSAGE_SIZE(buf);
	module_buffer[module_nr]->buffer[block_nr] = realloc(module_buffer[module_nr]->buffer[block_nr], DSMCC_MESSAGE_SIZE(buf));
	memcpy(module_buffer[module_nr]->buffer[block_nr], DSMCC_MESSAGE(buf), DSMCC_MESSAGE_SIZE(buf));
	return;
}

void handle_server_initate(unsigned char *buf, size_t len) {
	return;
}

void handle_dsmcc_message(unsigned char *buf, size_t len) {
	return;	/* It's without function yet */
	if (DSMCC_MESSAGE_TYPE(buf) == 0x3c) {
		handle_download_data_block(buf, len);
		// fprintf(stderr, "DSMCC: Download Data Block: 0x%x, Block-Nummer: 0x%x, Length: %ld\n", DSMCC_MODULE_ID(buf), DSMCC_BLOCKNR(buf), len);
	} else if ( DSMCC_MESSAGE_TYPE(buf) == 0x3b) {
		handle_server_initate(buf, len);
		// fprintf(stderr, "DSMCC Download-Server initiate: Message-type: %d, Length: %ld\n", DSMCC_MESSAGE_TYPE(buf), len);
		// DumpHex(buf, len);
	} else {
		fprintf(stderr, "DSMCC **UNKNOWN** Message-type: %d, Length: %ld\n", DSMCC_MESSAGE_TYPE(buf), len);
	}
	return;
}

/* Initialize basic data structures */
void init_dsmcc() {
	return;
}

void close_dsmcc() {
	return;
}

