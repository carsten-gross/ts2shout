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

// #define DEBUG
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
#define MAXDIRELEMENTS 200

/* Automatic variable, this way initialized with 0 */

module_buffer_t * module_buffer[65535];
dir_element_t dir_element[MAXDIRELEMENTS];
uint16_t num_elements = 0;

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
		single_module_buffer->buffer[block_nr] = NULL;
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

void add_direlement(uint64_t id, const char* filename, uint8_t* object_key, uint8_t object_key_length, modetype_t mode) {
	#ifdef DEBUG
	if (mode == FILETYPE) {
		fprintf(stderr, "add_direlement(): 0x%lx, %s (num: %d), object_key: 0x%02x%02x (length: %d)\n", id, filename, num_elements, object_key[0], object_key[1], object_key_length);
	} else {
		fprintf(stderr, "add_direlement(): 0x%lx, %s (num: %d) (Dirmode, object_key_len: %d)\n", id, filename, num_elements, object_key_length);
	}
	#endif
	if (object_key_length > 2) {
		assert(object_key_length <= 2);
		return;
	}
	/* Directory without object_key_length */
	if ( (mode = DIRTYPE || mode == TOPLEVELDIRTYPE) && object_key_length == 0) {
		for ( uint16_t i = 0; i < num_elements; i++) {
			if (dir_element[i].object_id == id) {
				strncpy(dir_element[i].filename, filename, sizeof(dir_element[i].filename)-1);
				dir_element[i].filename[sizeof(dir_element[i].filename)-1] = '\0';
				dir_element[i].mode = mode;
				return;
			}
		}
		/* Add new element */
		if ( num_elements < MAXDIRELEMENTS ) {
			dir_element[num_elements].object_key_length = object_key_length;
			dir_element[num_elements].object_id = id;
			strncpy(dir_element[num_elements].filename, filename, sizeof(dir_element[num_elements].filename)-1);
			dir_element[num_elements].filename[sizeof(dir_element[num_elements].filename)-1] = '\0';
			dir_element[num_elements].mode = mode;
	        num_elements++;
		}
		return;
	}
	/* change already existing element */
	for ( uint16_t i = 0; i < num_elements; i++) {
		if (dir_element[i].object_id == id) {
			if (memcmp(&dir_element[i].object_key, object_key, object_key_length) == 0) {
				strncpy(dir_element[i].filename, filename, sizeof(dir_element[i].filename)-1);
				dir_element[i].filename[sizeof(dir_element[i].filename)-1] = '\0';
				dir_element[i].mode = mode;
				return;
			}
		}
	}
	/* Add new element */
	if ( num_elements < MAXDIRELEMENTS ) {
		memcpy(&dir_element[num_elements].object_key, object_key, object_key_length);
		dir_element[num_elements].object_key_length = object_key_length;

        dir_element[num_elements].object_id = id;
        strncpy(dir_element[num_elements].filename, filename, sizeof(dir_element[num_elements].filename)-1);
        dir_element[num_elements].filename[sizeof(dir_element[num_elements].filename)-1] = '\0';
		dir_element[num_elements].mode = mode;
        num_elements++;
    }
	return;
}

char *get_filename(uint64_t id, uint8_t* object_key, uint8_t* object_key_length, modetype_t mode) {
	if (mode == FILETYPE) {
		for ( uint16_t i = 0; i < num_elements; i++) {
			if (dir_element[i].object_id == id) {
				memcpy(object_key_length, &dir_element[i].object_key_length, sizeof(uint8_t));
				memcpy(object_key, &dir_element[i].object_key, dir_element[i].object_key_length);
				return dir_element[i].filename;
			}
		}
	} else {
		/* Dirname */
	}
	return 0;
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
 * See ETSI TR 101 202 V1.2.1
 * buffer = pointer to first byte in module buffer
 * offset = initially 0, increment to next BIOP header if wanted for subsequent calls *
 * length = module length given in DSM-CC frame header
 * module_nr = module number
*/
uint32_t biop_file_message(unsigned char *buffer, size_t offset, size_t length, uint16_t module_nr) {
	uint32_t message_size;
	uint32_t remaining_message_size;
	uint32_t content_length;
	uint8_t	 object_key_length;
	uint8_t* object_key;
	uint8_t* object_id;
	unsigned char * current_pos;
	uint16_t object_info_length;
	uint8_t context_list_count;
	uint16_t context_data_length;
	uint8_t i = 0;
	uint8_t j = 0;
	char filename[255];
	FILE *f;
	modetype_t mode;
	static uint8_t errorshown = 0; /* Show filesystem errors only once */

	/* avoid wrong usage / crashes */
	if (length < 28 || offset > ( length - 28) ) {
		assert(length >= 28);
		assert(offset < ( length - 28));
		return 0;
	}
	if (buffer == NULL) {
		return 0;
	}
	current_pos = buffer + offset;
#if 0
	if (module_nr == 1 && offset > 0) {
		fprintf(stderr, "----- OFFSET %ld -----\n", offset);
		DumpHex(current_pos, 64);
	}
#endif
	if ( strncmp((char*)current_pos, "BIOP", 4) == 0 ) {
		/* MAGIC "BIOP" found */
	} else {
		return 0;
	}
	/* BIOP Version */
	if (current_pos[4] == 1 && current_pos[5] == 0 && current_pos[6] == 0 && current_pos[7] == 0) {
		/* Version, endianess and message type correct */
	} else {
		return 0;
	}
	current_pos = current_pos + 8;
	message_size = DSMCC_MODULE_FETCH32BITVAL(current_pos);
	remaining_message_size = message_size;
	object_key_length = current_pos[4];
	object_key = current_pos + 5;
	current_pos = current_pos + 5 + object_key_length;
	remaining_message_size = remaining_message_size - 5 - object_key_length;
	mode = FILETYPE;
	if (   current_pos[0] == 0 && current_pos[1] == 0 && current_pos[2] == 0 && current_pos[3] == 4 ) {
		if ( (strncmp((char*)current_pos + 4, "fil", 3) == 0 ) && current_pos[7] == 0 ) {
#ifdef DEBUG
			fprintf(stderr, "Module: 0x%x (size %ld), type File found, processing: %ld\n", module_nr, length, offset + message_size + 12);
#endif
			mode = FILETYPE;
			/* Found fil */
		} else if ( ( (strncmp((char*)current_pos + 4, "dir", 3) == 0) || (strncmp((char*)current_pos + 4, "srg", 3) == 0) )
					&& current_pos[7] == 0 ) {
#ifdef DEBUG
			if (object_key_length == 0) {
				fprintf(stderr, "Module: 0x%x (size %ld), type Directory (no object_key) found, processing: %ld\n", module_nr, length,
				offset + message_size + 12);
				DumpHex(current_pos, remaining_message_size);
			} else {
				fprintf(stderr, "Module: 0x%x (size %ld), type Directory (object_key_length: %d, object_key: 0x%02x) found, processing: %ld\n", module_nr, length,
				object_key_length, object_key[0], offset + message_size + 12);
				DumpHex(current_pos, remaining_message_size);
			}
			// DumpHex(current_pos, message_size);
#endif
			if (strncmp((char*)current_pos + 4, "dir", 3) == 0) {
				mode = DIRTYPE;
			} else {
				mode = TOPLEVELDIRTYPE;
			}
			/* Found dir */
		} else {
#ifdef DEBUG
			fprintf(stderr, "Module: 0x%x (size %ld), typ %c%c%c found, jumping to: %ld\n", module_nr, length, current_pos[4], current_pos[5], current_pos[6], offset + message_size + 12);
#endif
			return message_size + 12;
		}
	}
	/* Jump over 4 'fil\0' / 'dir\0' */
	current_pos = current_pos + 8;
	remaining_message_size -= 8;
	object_info_length = DSMCC_MODULE_FETCH16BITVAL(current_pos);
	remaining_message_size -= 2;
#ifdef DEBUG
	// DumpHex(current_pos + 2, object_info_length);
#endif
	object_id = current_pos + 2;
	current_pos = current_pos + object_info_length + 2;
	remaining_message_size = remaining_message_size - object_info_length;
	context_list_count = current_pos[0];
	remaining_message_size -= 1;
	current_pos = current_pos + 1;
	for (i = 0; i < context_list_count; i++) {
		current_pos = current_pos + 4; /* Skip context id */
		remaining_message_size -= 4;
		context_data_length = current_pos[0] <<8 | current_pos[1];
		for (j = 0; j < context_data_length; j++) {
			current_pos++;
			remaining_message_size--;
		}
	}
	current_pos += 4;
	remaining_message_size -= 4;
	// content_length = ((uint32_t)(current_pos[0]<<24 | current_pos[1]<<16 | current_pos[2]<<8 |current_pos[3]));
	content_length = DSMCC_MODULE_FETCH32BITVAL(current_pos);
	/*                        */
	/* Handle an ordinary file */
	/*                        */
	if (mode == FILETYPE) {
		current_pos += 4;
		remaining_message_size -= 4;
		uint64_t oo;
		uint8_t ok[2];
		uint8_t object_key_length = 0;
		memcpy(&oo, object_id, 8);
		if (get_filename(oo, ok, &object_key_length, FILETYPE)) {
			char dirname[255];
			for (uint8_t i = 0; i < object_key_length; i++) {
				// schmutzig, aber ist ja nur ein zwischenschritt
				snprintf((char*)&dirname[i*2], 3, "%02x", ok[i]);
			}
			#ifdef DEBUG
			fprintf(stderr, "Dirname %s, Filename %s, object_key: 0x%02x, object_key_length: %d\n", dirname, get_filename(oo, ok, &object_key_length, FILETYPE), *ok, object_key_length);
			#endif
			snprintf(filename, 255, CACHE_DIRECTORY "/Dir-%.10s-%.80s", dirname, get_filename(oo, ok, &object_key_length, FILETYPE));
		} else {
			snprintf(filename, 255, CACHE_DIRECTORY "/File-Module-0x%02x-ObjectId-0x%02x%02x%02x.data", module_nr, object_id[5], object_id[6], object_id[7]);
		}
		#ifdef DEBUG
		fprintf(stderr, "biop_file_message(): Module 0x%x, Offset %ld, BIOP File, writing to %s\n", module_nr, offset, filename );
		#endif
		f = fopen(filename, "w");
		if (! f) {
			/* No code beauty contest here, error is handled elsewhere */
			if (! errorshown) {
				output_logmessage("biop_file_message(): fopen(): %s: %s\n", filename, strerror(errno));
				errorshown = 1;
			}
			return message_size + 12;
		}
		fwrite( current_pos, 1, content_length, f);
		fclose(f);
	} else if ( mode == DIRTYPE || mode == TOPLEVELDIRTYPE ) {
		/*                    */
		/* Handle a directory */
		/*                    */
		uint16_t bindings_count = (current_pos[0]<<8) + current_pos[1];
		remaining_message_size -= 2;
		current_pos += 2;
		char comp_filename[255];
		char comp_kind[255];
		uint8_t object_info[8];
		if ( bindings_count == 0) {
			#ifdef DEBUG
			fprintf(stderr, "biop_file_message(): DIRECTORY, no bindings .. object key 0x%02x, object_key_len %d\n", object_key[0], object_key_length);
			#endif
		}
		for ( i = 0; i < bindings_count; i++) {
			#ifdef DEBUG
			fprintf(stderr, "biop_file_message(): DIRECTORY - Module: 0x%x,  length: %d, bindings (entries): %d\n", module_nr, remaining_message_size - 2, bindings_count);
			if (remaining_message_size > 2) {
				// DumpHex(current_pos, remaining_message_size - 2);
			}
			#endif
			uint8_t nameComponents_count = current_pos[0];
			current_pos += 1;
			remaining_message_size -= 1;
			for ( int j = 0; j < nameComponents_count; j++) {
				uint8_t id_length = current_pos[0];
				current_pos += 1;
				remaining_message_size -= 1;
				strncpy(comp_filename, (char *)current_pos, id_length);
				current_pos += id_length;
				remaining_message_size -= id_length;
			}
			uint8_t kind_length = DSMCC_MODULE_FETCH8BITVAL(current_pos);
			current_pos += 1;
			remaining_message_size -= 1;
			strncpy(comp_kind, (char *)current_pos, kind_length);
			current_pos += kind_length;
			remaining_message_size -= kind_length;
			current_pos += 1; // skip binding_type
			remaining_message_size -= 1;
			/* Also skip IOR::IOR */
			uint32_t type_id_length = DSMCC_MODULE_FETCH32BITVAL(current_pos);
			if (type_id_length > length) {
				assert(type_id_length < length);
				return 0;
			}
			current_pos = current_pos + 4 + type_id_length;
			remaining_message_size = remaining_message_size - 4 - type_id_length;
			uint32_t taggedProfiles_count = DSMCC_MODULE_FETCH32BITVAL(current_pos);
			assert(taggedProfiles_count < length);
			if (taggedProfiles_count > length) {
				assert(taggedProfiles_count < length);
				return 0;
			}
			current_pos += 4;
			remaining_message_size -= 4;
			for (int i = 0; i < taggedProfiles_count; i++) {
				current_pos += 4;  // profileId_tag
				remaining_message_size -= 4;
				uint32_t profile_data_length = DSMCC_MODULE_FETCH32BITVAL(current_pos);
				#ifdef DEBUG
				fprintf(stderr, "type_id_length: %d, taggedProfiles_count: %d, profile_data_length: %d\n", type_id_length, taggedProfiles_count, profile_data_length);
				#endif
				current_pos = current_pos + 4 + profile_data_length;
				remaining_message_size -= 4;
			}
			uint16_t objectInfo_length = DSMCC_MODULE_FETCH16BITVAL(current_pos);
			current_pos += 2; //
			remaining_message_size -= 2;
			memcpy(object_info, current_pos, fmin(objectInfo_length, 8));
			#ifdef DEBUG
			if (objectInfo_length > 0) {
				fprintf(stderr, "Dirinfo: Name: %s, Kind: %s, Object_info: 0x%02x%02x%02x, objectInfo_Length: %d\n", comp_filename, comp_kind, object_info[5], object_info[6], object_info[7], objectInfo_length);
			} else {
				fprintf(stderr, "Dirinfo: Name: %s, Kind: %s (no Object_info)\n", comp_filename, comp_kind);
			}
			#endif
			// Liste mit Filenamen
			if (strncmp(comp_kind, "fil", 3) == 0) {
				uint64_t oo;
				memcpy(&oo, object_info, 8);
				add_direlement(oo, comp_filename, object_key, object_key_length, FILETYPE);
			} else {
				uint64_t oo = 0;
				if (objectInfo_length > 0) {
					memcpy(&oo, object_info, 8);
				}
				add_direlement(oo, comp_filename, object_key, object_key_length, mode);
				#ifdef DEBUG
				DumpHex(current_pos, remaining_message_size);
				#endif
			}
			current_pos += objectInfo_length;
			remaining_message_size -= objectInfo_length;
		} /* for ( i = 0; i < bindings_count; i++) */
	}
	return message_size + 12;
}	

void check_module_complete(module_buffer_t* single_module_buffer) {
	uint16_t i = 0;
	// uint16_t module_nr = 0;
	size_t	length = 0;
	// char filename[255];
	int z_result;
	// FILE *f;
	// static uint8_t message_printed = 0;
	uint32_t message_offset = 0;
	if (single_module_buffer == NULL) {
		return;
	}
	// module_nr = single_module_buffer->module_number;
	for (i = 0; i < MAX_BLOCKNR; i++) {
		length += single_module_buffer->block_size[i];
	}
	if (length != single_module_buffer->data_size) {
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
	#if 0
	snprintf(filename, 255, CACHE_DIRECTORY "/Module-0x%x%s.bin", single_module_buffer->module_number, info);
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
	#endif
	message_offset = biop_file_message(current_pos, 0, current_length, single_module_buffer->module_number);
	while ( message_offset > 0 && message_offset < ( current_length - 28)  ) {
		size_t new_offset;
		new_offset = biop_file_message(current_pos, message_offset, current_length, single_module_buffer->module_number);
		message_offset = message_offset + new_offset;
	}
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
	#if 0
	#ifdef DEBUG
		fprintf(stderr, "Module_id  0x%x, Module size %d, Module_version 0x%x, module info_length %d\n",
			    DSMCC_MODULE_MODULE_ID(current_module),
				DSMCC_MODULE_MODULE_SIZE(current_module),
				DSMCC_MODULE_MODULE_VERSION(current_module),
				DSMCC_MODULE_INFO_LENGTH(current_module));
	#endif
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
	// return;	/* TODO: With this "return" no DSM-CC will be handled */
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

