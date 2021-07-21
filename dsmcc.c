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
#include <zlib.h>

#include "ts2shout.h"
#include "rds.h"
#include "dsmcc.h"

#define CACHE_DIRECTORY "/var/tmp/cache"

/* Automatic variable, this way initialized with 0 */

module_buffer_t * module_buffer[65535];

void cleanup_download_data_block(module_buffer_t* single_module_buffer, uint16_t block_nr) {
	if (single_module_buffer == NULL) {
		return;
	}
	if (block_nr > MAX_BLOCKNR) {
		output_logmessage("cleanup_download_data_block(): internal error, block_nr (%d) > %d, cannot handle\n", block_nr, MAX_BLOCKNR);
		return;
	}
	if (single_module_buffer->buffer[block_nr] != NULL) {
		free(single_module_buffer->buffer[block_nr]);
	}
	single_module_buffer->block_size[block_nr] = 0;
	single_module_buffer->valid = 0;
	return;
}

void cleanup_download_module_buffer(module_buffer_t* single_module_buffer) {
	uint16_t i = 0;
	if (single_module_buffer == NULL) {
		return;
	}
	for (i = 0; i < MAX_BLOCKNR; i++) {
		cleanup_download_data_block(single_module_buffer, i);
	}
	single_module_buffer->data_size = 0;
	single_module_buffer->max_blocknr = 0;
}

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
	if (block_nr > MAX_BLOCKNR) {
		output_logmessage("handle_download_data_block(): internal error, block_nr (%d) > %d, cannot handle\n", block_nr, MAX_BLOCKNR);
		return;
	}
	module_buffer[module_nr]->module_number = module_nr;
	if ( (DSMCC_MESSAGE_SIZE(buf) - 6) != module_buffer[module_nr]->block_size[block_nr]) {
		if ( module_buffer[module_nr]->block_size[block_nr] > 0 ) {
			cleanup_download_data_block(module_buffer[module_nr], block_nr);
		}
		module_buffer[module_nr]->block_size[block_nr] = DSMCC_MESSAGE_SIZE(buf) - 6;
		module_buffer[module_nr]->buffer[block_nr] = malloc(DSMCC_MESSAGE_SIZE(buf));
	}
	memcpy(module_buffer[module_nr]->buffer[block_nr], DSMCC_MESSAGE(buf), DSMCC_MESSAGE_SIZE(buf) - 6);
	return;
}

/* Decode BIOP Files
 * See ETSI TR 101 202 V1.2.1 */
void biop_file_message(unsigned char *buffer, size_t length) {
	// uint32_t message_size;
	uint32_t content_length; 
	uint8_t	 object_key_length;
	unsigned char * object_key;
	unsigned char * current_pos;
	uint16_t object_info_length;
	uint8_t context_list_count;
	uint16_t context_data_length;
	uint8_t i = 0;
	uint8_t j = 0;
	char filename[255];
	FILE *f; 
	if (buffer == NULL) {
		return;
	}
	if (buffer[0] == 'B' && buffer[1] == 'I' && buffer[2] == 'O' && buffer[3] == 'P') {
		/* MAGIC "BIOP" found */
	} else {
		return;
	}
	/* BIOP Version */
	if (buffer[4] == 1 && buffer[5] == 0 && buffer[6] == 0 && buffer[7] == 0) {
		/* Version, endianess and message type correct */
	} else {
		return;
	}
	// message_size = ((uint32_t)(buffer[8]<<24 | buffer[9]<<16 | buffer[10]<<8 | buffer[11]));
	object_key_length = buffer[12];
	object_key = buffer + 13;
	current_pos = buffer + 13 + object_key_length;
#ifdef DEBUG
	fprintf(stderr, "Expect 0x000004, 'fil '\n"); 
	DumpHex(current_pos, 8); 
#endif
	if (   current_pos[0] == 0 && current_pos[1] == 0 && current_pos[2] == 0 && current_pos[3] == 4 
		&& current_pos[4] == 'f' && current_pos[5] == 'i' && current_pos[6] == 'l' && current_pos[7] == 0) {
		/* Found fil */
	} else {
		return;
	}
	/* Jump over 4 'fil\0' */
	current_pos = current_pos + 8;
	/* read object info length */
	object_info_length = current_pos[0]<<8 | current_pos[1];
	current_pos = current_pos + object_info_length + 2;
	context_list_count = current_pos[0];
	current_pos = current_pos + 1;
	for (i = 0; i < context_list_count; i++) {
		current_pos = current_pos + 4; /* Skip context id */
		context_data_length = current_pos[0] <<8 | current_pos[1];
		for (j = 0; j < context_data_length; j++) {
			current_pos++; 
		}
	}
	current_pos += 4;
	content_length = ((uint32_t)(current_pos[0]<<24 | current_pos[1]<<16 | current_pos[2]<<8 |current_pos[3]));
	current_pos = current_pos + 4;
	snprintf(filename, 255, CACHE_DIRECTORY "/0x%02x%02x%02x.data", object_key[0], object_key[1], object_key[2]);
#ifdef DEBUG
	fprintf(stderr, "check_module_complete(): BIOP File, writing to %s!\n", filename);
#endif
	f = fopen(filename, "w");
	if (! f) {
		/* No code beauty contest here, error is handled elsewhere */
		perror("biop_file_message(): fopen()");
		return;
	}
	fwrite( current_pos, 1, content_length, f); 
	fclose(f);
	return;
}	

void check_module_complete(module_buffer_t* single_module_buffer) {
	uint16_t i = 0;
	size_t	length = 0;
	char filename[255];
	int z_result;
	FILE *f;
	static uint8_t message_printed = 0; 
	if (single_module_buffer == NULL) {
		return;
	}
	for (i = 0; i < MAX_BLOCKNR; i++) {
		length += single_module_buffer->block_size[i];
	}
	if (length != single_module_buffer->data_size) {
		// fprintf(stderr, "check_module_complete(): lenght %ld != single_module_buffer->data_size %ld\n", length, single_module_buffer->data_size); 
		return;
	}
	/* Write to disk for the time being */
	/* uncompress on the fly */
	unsigned char * compressed_data;
	unsigned char * current_pos;
	unsigned char * uncompressed_data;
	char info[15]; 
	size_t uncompressed_length = 10 * length; /* Assume factor 10 (3 and 5 are not sufficient) */
	size_t current_length = 0;  
	compressed_data = alloca(length + 1);  /* use stack */
	current_pos = compressed_data; 
	uncompressed_data = alloca(uncompressed_length + 1); 
	/* Try to uncompress */
	for (i = 0; i < MAX_BLOCKNR; i++) {
		if (single_module_buffer->block_size[i] > 0) {
			if ( single_module_buffer->module_number == 1) {
#ifdef DEBUG
				fprintf(stderr, "Module 1, Block-Nr %d, Size: %d\n", i, single_module_buffer->block_size[i]);
#endif 
			}
			memcpy(current_pos, single_module_buffer->buffer[i], single_module_buffer->block_size[i]);
			current_pos += single_module_buffer->block_size[i];
		} else {
			continue; 
		}
	}
	z_result = uncompress(uncompressed_data, &uncompressed_length, compressed_data, length);
	if (z_result == Z_OK) {
		snprintf(info, 14, "-unzipped"); 
		current_pos = uncompressed_data; 
		current_length = uncompressed_length; 
	} else {
		// fprintf(stderr, "Cannot uncompress module 0x%x: %d\n", single_module_buffer->module_number, z_result);
		strcpy(info, "");
		current_pos = compressed_data; 
		current_length = length; 
	}
	snprintf(filename, 255, CACHE_DIRECTORY "/0x%x%s.bin", single_module_buffer->module_number, info);
#ifdef DEBUG
	fprintf(stderr, "check_module_complete(): Module 0x%x complete, writing to %s!\n", single_module_buffer->module_number, filename);
#endif
	f = fopen(filename, "w"); 
	if (! f) {
		if (! message_printed) {
			output_logmessage("Write problem in " CACHE_DIRECTORY ". This message will only be logged once\n"); 
			output_logmessage("fopen file `%s' for writing in check_module_complete(): %s\n", filename, strerror(errno));
			message_printed = 1;
		}
		cleanup_download_module_buffer(single_module_buffer); 
		return;
	}
	fwrite( current_pos, 1, current_length, f); 
	fclose(f);
	biop_file_message(current_pos, current_length); 
	cleanup_download_module_buffer(single_module_buffer); 
	return; 
}



void handle_server_initate(unsigned char *buf, size_t len) {
	uint16_t module_count;
	uint16_t i = 0;
	uint16_t j = 0;
	unsigned char * current_module;
	if (DSMCC_MESSAGE_TYPE(buf) != 0x3b) {
		output_logmessage("handle_server_initiate(): internal error, called with wrong message type 0x%x\n", DSMCC_MESSAGE_TYPE(buf));
		return;
	}
	if (DSMCC_PROTOCOL_DISCRIMINATOR(buf) != 0x11) {
		return;
	}
	if (DSMCC_TABLE_EXTENSION(buf) == 0) {
		return;
	}
	module_count = DSMCC_DII_MODULE_COUNT(buf);
	//fprintf(stderr, "handle_server_initate(): transaction_id 0x%x, message_id 0x%x, module_count 0x%x\n", transaction_id, message_id, module_count);
	current_module = DSMCC_DII_MODULES_START(buf); 

	for (i = 0; i < module_count; i++) {
		uint16_t module_nr; 
#ifdef DEBUG
		fprintf(stderr, "Module_id  0x%x, Module size %d, Module_version 0x%x, module info_length %d\n", 
			    DSMCC_MODULE_MODULE_ID(current_module),
				DSMCC_MODULE_MODULE_SIZE(current_module),
				DSMCC_MODULE_MODULE_VERSION(current_module),
				DSMCC_MODULE_INFO_LENGTH(current_module));
#endif
		module_nr = DSMCC_MODULE_MODULE_ID(current_module);
		/* Spec says that a list of descriptors follows in length DSMCC_MODULE_INFO_LENGTH(current_module) */
		if (module_buffer[module_nr]) {
			module_buffer[module_nr]->data_size = DSMCC_MODULE_MODULE_SIZE(current_module);
			check_module_complete(module_buffer[module_nr]); 
		}
		current_module = current_module + DSMCC_MODULE_INFO_LENGTH(current_module) + 8;
		j += DSMCC_MODULE_INFO_LENGTH(current_module); 
		if ( j > len) {
			output_logmessage("handle_server_initate(): invalid MPEG Frame, j (%d) > len (%d)\n", j, len);
			return;
		}
	}
	return;
}

void handle_dsmcc_message(unsigned char *buf, size_t len) {
	/* If you want to test it ... uncomment the return */
	return;	/* TODO: With this "return" no DSM-CC will be handled */
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

