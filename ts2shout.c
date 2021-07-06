/*

	ts2shout.c
	(C) Carsten Gross <carsten@siski.de> 2018-2021
	reworked from dvbshout.c written by
	(C) Dave Chapman <dave@dchapman.com> 2001, 2002.
	(C) Nicholas J Humfrey <njh@aelius.com> 2006.
	
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


#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <curl/curl.h>
#include <assert.h>

#include "ts2shout.h"
#include "rds.h"

#define XSTR(s) STR(s)
#define STR(s) #s

int Interrupted=0;        /* Playing interrupted by signal? */
int channel_count=0;      /* Current listen channel count */

uint8_t shoutcast=1;      /* Send shoutcast headers? */
uint8_t	logformat=1;      /* Apache compatible output format */
uint8_t	cgi_mode=0;       /* Are we running as CGI programme? This is set if there is QUERY_STRING set in the environment */

uint64_t frame_count=0;	  /* ts-Frame number (used for debugging) */

static const long int mb_conversion = 1024 * 1024;

ts2shout_channel_t *channel_map[MAX_PID_COUNT];
ts2shout_channel_t *channels[MAX_CHANNEL_COUNT];

/* Structure that keeps track of connected EIT frames */
section_aggregate_t *eit_table;
section_aggregate_t *sdt_table;


/* Global application structure */
programm_info_t *global_state;

static void signal_handler(int signum)
{
	if (signum != SIGPIPE) {
		Interrupted=signum;
	}
	signal(signum,signal_handler);
}

static void parse_args(int argc, char **argv)
{
	/* TODO ... improve command line handling */
	int i = 0;
	for (i = 0; i < argc; i++) {
		if (strcmp("noshout", argv[i]) == 0) {
			shoutcast = 0;
		}
		if (strcmp("ac3", argv[i]) == 0) {
			global_state->want_ac3 = 1;
		}
		if (strcmp("rds", argv[i]) == 0) {
			global_state->prefer_rds = 1;
		}
	}
}

void cleanup_mpeg_string(char *string) {
	uint32_t i = 0;
	if (string == NULL)
		return;
	for (i = 0; i < strlen(string); i++) {
		/* replace control character "LF/CR" by Space */
		if ((unsigned char)string[i] == 0x8a) {
			string[i] = 0x20;
		}
	}
	return;
}


/* Output logmessage in apache errorlog compatible format */
void output_logmessage(const char *fmt, ... ) {
	char s[STR_BUF_SIZE];
	char current_time_fmt[STR_BUF_SIZE];
	char current_time[STR_BUF_SIZE];
	va_list argp;
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	strftime(current_time_fmt, STR_BUF_SIZE, "%a %b %d %H:%M:%S.%%6.6d %Y", localtime(&t.tv_sec));
	snprintf(current_time, STR_BUF_SIZE, current_time_fmt, (t.tv_nsec / 1000));
	va_start(argp, fmt);
	vsnprintf(s, STR_BUF_SIZE, fmt, argp);
	va_end(argp);
	if ( 1 == logformat ) {
		fprintf(stderr, "[%s] [ts2shout:info] [pid %d] %s",
			current_time, getpid(), s);
	} else {
		fprintf(stderr, "%s", s);
	}
	return;
}

static void ts_continuity_check( ts2shout_channel_t *chan, int ts_cc )
{
	if (chan->continuity_count != ts_cc) {
	
		// Only display an error after we gain sync
		if (chan->synced) {
			output_logmessage("ts_continuity_check: TS continuity error (pid: %d)\n", chan->pid );
			chan->synced=0;
		}
		chan->continuity_count = ts_cc;
	}
	chan->continuity_count++;
	if (chan->continuity_count >= 16)
		chan->continuity_count = 0;
}


static void extract_pat_payload(unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes ) {
	unsigned char* start = NULL;
	start = pes_ptr + start_of_pes;
	unsigned int possible_pmt = 0;
#ifdef DEBUG
    fprintf (stderr, "PAT: Found data, table 0x%2.2d (Section length %d), transport_stream_id %d, section %d, last section %d\n",
		PAT_TABLE_ID(start),
		PAT_SECTION_LENGTH(start),
		PAT_TRANSPORT_STREAM_ID(start),
		PAT_SECTION_NUMBER(start),
		PAT_LAST_SECTION_NUMBER(start));
#endif
	unsigned char* one_program = PAT_PROGRAMME_START(start);
	/* tvheadend and vdr rewrite PAT, but some mp2t serving systems only remove PMT pids and leave PAT as it is.
	 * Therefore we have to scan PAT for alle PMT pids. If there are also more than one PMT in the stream a
	 * "random" (the first seen PMT) program is selected. */
	// if (! channel_map[PAT_PROGRAMME_PMT(one_program)] ) { // This was here before, but does not work anymore, because we allow more than one PMT result
	/* Add PMT if we don't have a valid transport_stream_id already */
	if ( global_state->transport_stream_id  != PAT_TRANSPORT_STREAM_ID(start)) {
		if (crc32(start, PAT_SECTION_LENGTH(start) + 3) == 0) {
			unsigned int i = 0;
#ifdef DEBUG
			// DumpHex(one_program, PAT_SECTION_LENGTH(start) - 8);
#endif
			/* Scan for possible, valid PMTs */
			global_state->transport_stream_id = PAT_TRANSPORT_STREAM_ID(start);
			for (i = 0; i < ( PAT_SECTION_LENGTH(start) - 8 - 2 /* CRC */ ); i += 4) {
				if (PAT_PROGRAMME_PMT( (one_program + i) ) > 0x11) { // Only add normal tables, not NIT et.al.
					add_channel(CHANNEL_TYPE_PMT, PAT_PROGRAMME_PMT( (one_program + i) ));
					possible_pmt += 1;
				}
			}
		}
		output_logmessage("extract_pat_payload(): Added %d possible PMT id(s) with transport_stream_id: %d.\n", possible_pmt, global_state->transport_stream_id);
	}
	return;
}

/* Let's hate software patents. This table is guessed out of real world radio DVB-S reception
 * It's not correct, but if someone wants this fixed, please provide me a description of how
 * the streaming parameters are read out of the LATM stream or mpeg-ts stream PMT.
 */

static void set_latm_parameters(uint8_t aac_profile) {
	global_state->latm_magic1 = 0x56;
	if (aac_profile == 0x51 ) {
		global_state->sr = 48000;
		global_state->br = 128;
		global_state->latm_magic2 = 0xe1;
	} else if (aac_profile == 0x52 ) {
		global_state->sr = 48000;
		global_state->br = 256;
		global_state->latm_magic2 = 0xe2;
	} else if (aac_profile == 0x60 ) {
		global_state->sr = 48000;
		if (global_state->br == 0) {
			global_state->br = 48;
		}
		global_state->latm_magic2 = 0xe1;
	} else {
		global_state->sr = 48000;
		if (global_state->br == 0) {
			global_state->br = 128;
		}
		global_state->latm_magic2 = 0xe1;
		output_logmessage("Sorry, no configuration for AAC profile (id=0x%x) found. All values guessed.\n");
	}
	return;
}

/* Get info about an available media stream (we want mp1/mp2/mp4 or AC-3) */

static void add_payload_from_pmt(unsigned char *pmt_stream_info_offset, unsigned char *start) {
	unsigned int stream_type;
	char *stream_type_name = "";
	enum {
		NO_AUDIO_STREAM,
		CHECK_DESCRIPTOR,
		AUDIO_STREAM } audio_all_checks = NO_AUDIO_STREAM; /* Local use only: 0 = no audio found, 1 = unsufficent descriptor data, 2 = OK */
	stream_type = PMT_STREAM_TYPE(pmt_stream_info_offset);
	switch ( stream_type ) {
		case 0x03:	/* MPEG 1 audio */
			if (! global_state->want_ac3 ) {
				stream_type_name = "MPEG 1";
				global_state->stream_type = AUDIO_MODE_MPEG;
				audio_all_checks = AUDIO_STREAM;
			}
			break;
		case 0x04:	/* MPEG 2 audio */
			if (! global_state->want_ac3 ) {
				stream_type_name = "MPEG 2";
				global_state->stream_type = AUDIO_MODE_MPEG;
				audio_all_checks = AUDIO_STREAM;
			}
			break;
		case 0x0f:	/* MPEG 2 AAC */
			if (! global_state->want_ac3 ) {
				stream_type_name = "AAC";
				global_state->stream_type = AUDIO_MODE_AAC;
				audio_all_checks = AUDIO_STREAM;
			}
			break;
		case 0x11:  /* MPEG 4 AAC LATM */
			if (! global_state->want_ac3 ) {
				stream_type_name = "HE-AAC";
				global_state->stream_type = AUDIO_MODE_AACP;
				audio_all_checks = AUDIO_STREAM;
			}
			break;
		case 0x06:	/* AC-3 (only if wanted *) */
			if ( global_state->want_ac3 ) {
				stream_type_name = "AC-3";
				global_state->stream_type = AUDIO_MODE_AC3;
				audio_all_checks = CHECK_DESCRIPTOR;
			} else {
				return;
			}
			break; 
		default:
			return;
			break;
	}
	/* We found a supported media stream */
	/* Scan descriptors */
	{
		unsigned int offset = 0;
#ifdef DEBUG
		fprintf(stderr, "Searching PMT descriptors: Length of descriptors %d\n", PMT_INFO_LENGTH(pmt_stream_info_offset));
#endif
		while (offset < PMT_INFO_LENGTH(pmt_stream_info_offset)) {
			unsigned char * descriptor_pointer = PMT_FIRST_STREAM_DESCRIPTORP(pmt_stream_info_offset) + offset;
			offset = offset + DESCRIPTOR_LENGTH (descriptor_pointer) + 2; /* The 2 is the static size of the offset and the tag byte itself */
#ifdef DEBUG
			fprintf(stderr, "Searching PMT descriptors, current type 0x%x: next descriptor @%d\n", DESCRIPTOR_TAG(descriptor_pointer), offset);
#endif
			/* Maximum Bitrate descriptor, found description in wireshark and on the internet */
			if ( DESCRIPTOR_TAG(descriptor_pointer) == 0x0e ) {
				int bitrate;
				bitrate = ((descriptor_pointer[2] & 0x3f)<<16) + (descriptor_pointer[3]<<8) + descriptor_pointer[4];
				bitrate = bitrate * 50;
				global_state->br = ( bitrate * 8 ) / 1024;
				output_logmessage("add_payload_from_pmt(): %s maximum bitrate %.1f KByte/s (%.1f KBit/s)\n", stream_type_name, (float)bitrate/1024, ((float)bitrate * 8) /1024);
			}
			/* AC-3 Descriptor */
			if ( global_state->want_ac3 && DESCRIPTOR_TAG(descriptor_pointer) == 0x6a ) {
				output_logmessage("add_payload_from_pmt(): Found AC-3 audio descriptor for PID %d\n", PMT_PID(pmt_stream_info_offset));
				if ( audio_all_checks == CHECK_DESCRIPTOR && global_state->stream_type == AUDIO_MODE_AC3) {
					audio_all_checks = AUDIO_STREAM;
				}
				/* TODO parse AC-3 parameters out of fields */
			}
			/* AAC descriptor */
			if ( DESCRIPTOR_TAG(descriptor_pointer) == 0x7c ) {
				uint8_t aac_profile;
				aac_profile = descriptor_pointer[2];
				set_latm_parameters(aac_profile);
				output_logmessage("add_payload_from_pmt(): Audio `%s'\n", aac_profile_name(aac_profile));
			}
			if ( DESCRIPTOR_TAG(descriptor_pointer) == 0x0a ) {
				unsigned char language[4];
				memcpy(language, descriptor_pointer + 2, 3);
				language[3] = 0;
				output_logmessage("add_payload_from_pmt(): stream language `%s'\n", language);
				/* ger, deu, FRA, fre... oh, this is quite a mess even if you look only on german and french */
			}
		}
	}
	/* If all parameters are ok, add the payload stream */
	if ( audio_all_checks == AUDIO_STREAM ) {
		global_state->service_id = PMT_PROGRAM_NUMBER(start);
		global_state->mime_type = mime_type(global_state->stream_type);
		global_state->payload_added = 1;
		output_logmessage("add_payload_from_pmt(): Found %s audio stream in PID %d (service_id %d)\n", stream_type_name, PMT_PID(pmt_stream_info_offset), global_state->service_id);
		add_channel(CHANNEL_TYPE_PAYLOAD, PMT_PID(pmt_stream_info_offset));
	}
	return;
}


/* Get stream info out of the PMT (program map table). We are only interested in radio mp1/mp2/aac streams or
 * alternativly for AC-3. */

static void extract_pmt_payload(unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes ) {
	unsigned char* start = NULL;
	unsigned int found_streams_counter = 0;
	/* Only check for possible streaming payload in PMT if not one is added yet */
	if ( global_state->payload_added) {
		return;
	}
	start = pes_ptr + start_of_pes;
#ifdef DEBUG
    fprintf (stderr, "PMT: Found data, table 0x%2.2x (Section length %d), program number %d, section %d, last section %d, INFO Length %d\n",
		PMT_TABLE_ID(start),
		PMT_SECTION_LENGTH(start),
		PMT_PROGRAM_NUMBER(start),	
		PMT_SECTION_NUMBER(start),
		PMT_LAST_SECTION_NUMBER(start),
		PMT_PROGRAMME_INFO_LENGTH(start));
#endif
	/* Look into table 0x02 containing the PID to be read */
	if ( PMT_TABLE_ID(start) == 2) {
		/* Check crc32 to avoid checking it later on */
		if (crc32(start, PAT_SECTION_LENGTH(start) + 3) != 0) {
			output_logmessage("extract_pmt_payload: crc32 does not match %d found, 0 expected\n", crc32(start, PAT_SECTION_LENGTH(start) + 3));
			return;
		}
		if (PMT_SECTION_NUMBER(start) == 0 && PMT_LAST_SECTION_NUMBER(start) == 0) {
			unsigned char* pmt_stream_info_offset = PMT_DESCRIPTOR(start);
			/* Search for stream description */
			while ((pmt_stream_info_offset != NULL) && found_streams_counter < 3) {
				if (! channel_map[PMT_PID(pmt_stream_info_offset)]) {
		            /* right transport_id? */
					add_payload_from_pmt(pmt_stream_info_offset, start);
					found_streams_counter += 1;
					if (global_state->payload_added > 0) {
						pmt_stream_info_offset = NULL;
						global_state->service_id = PMT_PROGRAM_NUMBER(start);
						pmt_stream_info_offset = NULL;
					} else {
						pmt_stream_info_offset = PMT_FIRST_STREAM_DESCRIPTORP(pmt_stream_info_offset) + PMT_INFO_LENGTH(pmt_stream_info_offset);
#ifdef DEBUG
						fprintf(stderr, "PMT: Found other stream with PID %d, stream type %d\n", PMT_PID(pmt_stream_info_offset), PMT_STREAM_TYPE(pmt_stream_info_offset));
#endif
					}
				} else {
					/* check next */
					pmt_stream_info_offset = PMT_FIRST_STREAM_DESCRIPTORP(pmt_stream_info_offset) + PMT_INFO_LENGTH(pmt_stream_info_offset);
#ifdef DEBUG
	fprintf(stderr, "PMT: Found other stream with PID %d, stream type %d\n", PMT_PID(pmt_stream_info_offset), PMT_STREAM_TYPE(pmt_stream_info_offset));
#endif
					found_streams_counter += 1;
				}
			}	
		}
	}
}


/* Sometimes more than one small table has already been collected in our
 * aggregation buffer. This function simply grabs the next table in the buffer,
 * returns it, and moves the rest of the buffer contents to the beginning of
 * the buffer. This function can be called as often as needed, it returns 0 if
 * there is nothing and 1 if the data was successfully extracted from the
 * buffer. */

uint8_t	fetch_next(section_aggregate_t* aggregation, uint8_t *buffer, uint8_t* size) {
	uint16_t section_length = EIT_SECTION_LENGTH(aggregation->buffer);
	if (section_length == 0
		|| section_length + 3 > aggregation->offset
		|| section_length + 3 > EIT_BUF_SIZE) {
		/* not enough data in buffer */
		*size = 0;
		return 0;
	}
	if (crc32(aggregation->buffer, section_length + 3) != 0) {
		/* not valid */
		#ifdef DEBUG
		fprintf(stderr, "fetch_next(): Found data, but crc32 is wrong %d!=0", crc32(aggregation->buffer, section_length + 3) );
		#endif
		*size = 0;
		return 0;
	}
	#ifdef DEBUG
	fprintf(stderr, "fetch_next(): Found valid data with size %d\n", section_length);
	#endif
	/* Copy into return buffer. If it's not given the function can be used to check for validity of buffer! */
	if (buffer) {
		memcpy(buffer, aggregation->buffer, section_length + 3);
		*size = section_length;
		/* move the rest around */
		memmove(aggregation->buffer, aggregation->buffer + section_length + 3, EIT_BUF_SIZE - (section_length + 3));
		aggregation->offset = aggregation->offset - (section_length + 3);
	}
	return 1;
}
/*   * * *  Collect a table (SDT, EIT) out of many MPEG frames * * *
 * If an information block doesn't fit into an mpeg-ts frame it is continued in a next frame. The information is
 * directly attached after the PID (TS_HEADER_SIZE = 4 Byte offset). It is possible that there is a multi-ts-frame continuation.
 * Even worse: EIT frames are joined directly together. Even inbetween the MPEG TS frame. No stuffing bytes
 * If mpeg packet says "payload start" the pointer in 4th byte tells where the table start is. This is quite burried in
 * the documentation.
 * For the time being we search for stuffing at the end of a mpeg table: stuffing bytes, then we assume the next mpeg packet starts over.
 * But we respect the pointer anyway (because our access macro is quite failsafe).
 * No stuffing bytes? We check the CRC32, if it is valid, the table is valid: The next one is attached directly.
 * TODO Currently the implementation doesn't fetch all information reliably.
 * This is an issue with mpeg ts stream provided by vdr.
 * tvheadend prepares the mpeg ts stream in another way and we have no issues with it. */

uint8_t collect_continuation(section_aggregate_t* aggregation, unsigned char *pes_ptr, size_t pes_len, int start_of_pes, unsigned char* ts_full_frame, enum_channel_type type) {

	/* start of the part of the packet we are interested in */
	unsigned char* start = pes_ptr + start_of_pes;

	/* We are currently aggregating mpeg packets for a continuated frame? */
 	if (aggregation->buffer_valid == 0 && aggregation->continuation > 0 ) { // && (TS_PACKET_PAYLOAD_START(ts_full_frame) == 0) )  {
		/* yes */
#ifdef DEBUG
		fprintf(stderr, "%s: continued frame: offset: %d, counter: %d, section_length: %d, payload_start: %d (Pointer: %d)\n",
			channel_name(type), aggregation->offset, aggregation->counter, aggregation->section_length,
			TS_PACKET_PAYLOAD_START(ts_full_frame), TS_PACKET_POINTER(ts_full_frame));
			DumpHex(start, TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes);
#endif
		memcpy(aggregation->buffer + aggregation->offset, start, TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes);
		aggregation->offset += TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes; /* Offset for next TS packet */
		aggregation->counter += 1;
		/* 4 = size of crc32 (not included in section length) */
		if ((aggregation->offset - TS_HEADER_SIZE - start_of_pes - 4) >= aggregation->section_length ) {
			/* Section is finished, finally */
			aggregation->buffer_valid = 1;
			aggregation->continuation = 0;
#ifdef DEBUG
			fprintf(stderr, "%s: finished multi-frame table after offset %d, counter: %d\n",
				channel_name(type), aggregation->offset, aggregation->counter);
#endif
			/* Check wether there is some leftover */
			aggregation->ob_used = aggregation->offset - aggregation->section_length;
			/* Stuffing bytes, or size wrong? */
			if ((start[TS_PACKET_SIZE - aggregation->ob_used + 3] == 0xff && start[TS_PACKET_SIZE - aggregation->ob_used + 4] == 0xff)
				|| (aggregation->ob_used > TS_PACKET_SIZE )
			) {
				/* nothing */
				aggregation->ob_used = 0;
			} else {
				/* The 4 is the crc32 that is not mentioned in section length */
				memcpy(aggregation->offset_buffer, start + 3 + TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes - aggregation->ob_used, aggregation->ob_used);
				if (aggregation->ob_used < 4) {
					aggregation->ob_used = 0;
				} else {
					aggregation->ob_used -= 3;
				}
	#ifdef DEBUG
				fprintf(stderr, "Leftover in continued frame ....\n");
				DumpHex(aggregation->offset_buffer, aggregation->ob_used);
	#endif
			}
			return 1;
		} else {
			if ( TS_PACKET_PAYLOAD_START(ts_full_frame) == 1) {
			#ifdef DEBUG
				fprintf(stderr, "%s: I'm at offset %d, and a new payload start is there... interrupting\n", channel_name(type), aggregation->offset);
			#endif
				aggregation->buffer_valid = 0;
				aggregation->continuation = 1;
				aggregation->counter = 1;
				aggregation->section_length = EIT_SECTION_LENGTH((start + TS_PACKET_POINTER(ts_full_frame)));
				aggregation->offset = TS_PACKET_SIZE - start_of_pes - 4 - TS_PACKET_POINTER(ts_full_frame) ; /* Offset for next TS packet */
				/* Copy first frame directly into buffer */
				memcpy(aggregation->buffer, pes_ptr + start_of_pes + TS_PACKET_POINTER(ts_full_frame), TS_PACKET_SIZE - start_of_pes - TS_PACKET_POINTER(ts_full_frame) );
				/* Packet is not finished yet, it cannot be handled now */
			#ifdef DEBUG
				fprintf (stderr, "%s: Found multi-frame-table 0x%2.2x (last table 0x%2.2x), Section-Length: %d, Section: %d, payload_start: %d (Pointer %d)\n",
					channel_name(type), EIT_PACKET_TABLEID((start + TS_PACKET_POINTER(ts_full_frame))), EIT_PACKET_LAST_TABLEID((start + TS_PACKET_POINTER(ts_full_frame))),
					EIT_SECTION_LENGTH((start + TS_PACKET_POINTER(ts_full_frame))), EIT_SECTION_NUMBER((start + TS_PACKET_POINTER(ts_full_frame))),
					TS_PACKET_PAYLOAD_START(ts_full_frame), TS_PACKET_POINTER(ts_full_frame));
				DumpHex(start, 183);
			#endif
				return 0;
			}
			// This should not happen, because the standard recommends a maximum size of ~4K */
			if (aggregation->offset + TS_PACKET_SIZE - TS_HEADER_SIZE > EIT_BUF_SIZE - TS_PACKET_SIZE ) {
				output_logmessage("collect_continuation: Maximum Data-Chunk Size of %d characters " \
				    "exceeded by MPEG-Transport-Stream. Did read %d continued packets (%d bytes)\n",
					EIT_BUF_SIZE - TS_PACKET_SIZE, aggregation->counter, aggregation->offset);
				// reset internal buffer
				memset(aggregation, 0, sizeof(section_aggregate_t));
			}
			return 0;
		}
		/* Move start of packet to helper buffer for long frames */
	}
	if ( aggregation->buffer_valid == 0) {
		/* Check wether we have a *valid* leftover from last TS packet */
		if ( aggregation->continuation == 0 ) {
			if (aggregation->ob_used > 5) {
				uint8_t size = 0;
				/* special operation */
				/* Just assume that this frame is longer then an mpeg frame ... */
				aggregation->continuation = 1;
				aggregation->counter = 1;
				aggregation->section_length = EIT_SECTION_LENGTH(aggregation->offset_buffer);
				aggregation->offset = aggregation->ob_used;
				memcpy(aggregation->buffer, aggregation->offset_buffer, aggregation->ob_used);
				aggregation->ob_used = 0;
				/* Copy the rest of the mpeg frame */
				memcpy(aggregation->buffer + aggregation->offset, pes_ptr + start_of_pes, TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes );
				aggregation->offset += TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes;

				#ifdef DEBUG
					fprintf (stderr, "%s: Found continued multi-frame-table from last frame 0x%2.2x (data %d byte), Section-Length: %d, Section: %d\n",
				channel_name(type), EIT_PACKET_TABLEID(start), aggregation->offset, EIT_SECTION_LENGTH(aggregation->buffer),
				EIT_SECTION_NUMBER(aggregation->buffer));
				DumpHex(aggregation->buffer, aggregation->offset);
				#endif
				/* Returns 0 on valid frame, 1 on success */
				return fetch_next(aggregation, NULL, &size);
			}
		}
		/* A short EIT frame < TS_PACKET_SIZE length, take care of the crc32 (not included in section_length)! */
		if ( aggregation->continuation == 0 && EIT_SECTION_LENGTH(start) < (TS_PACKET_SIZE - TS_HEADER_SIZE - 4 )   ) {
			aggregation->buffer_valid = 1;
			aggregation->continuation = 0;
			aggregation->counter = 1;
			aggregation->section_length = EIT_SECTION_LENGTH(start);
			aggregation->offset = 0;
			memcpy(aggregation->buffer, start, TS_PACKET_SIZE - start_of_pes );
	#ifdef DEBUG
			fprintf (stderr, "%s: Single frame-table 0x%2.2x (last table 0x%2.2x), Section-Length: %d (%d), Section: %d, payload_start: %d (Pointer: %d)\n",
				channel_name(type), EIT_PACKET_TABLEID(start), EIT_PACKET_LAST_TABLEID(start), EIT_SECTION_LENGTH(start), TS_PACKET_SIZE - start_of_pes, EIT_SECTION_NUMBER(start),
				TS_PACKET_PAYLOAD_START(ts_full_frame), TS_PACKET_POINTER(ts_full_frame));
	#endif
		} else if ( ( EIT_SECTION_LENGTH((start + TS_PACKET_POINTER(ts_full_frame))) >= (TS_PACKET_SIZE - TS_HEADER_SIZE - 4))
					&& ( 0 == aggregation->continuation )
					&& (TS_PACKET_PAYLOAD_START(ts_full_frame) == 1) ) {
			/* Start of a long frame, will be continued in next frame */
			aggregation->buffer_valid = 0; /* It's not valid @ the moment */
			aggregation->continuation = 1;
			aggregation->counter = 1;
			aggregation->section_length = EIT_SECTION_LENGTH((start + TS_PACKET_POINTER(ts_full_frame)));
			aggregation->offset = TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes - TS_PACKET_POINTER(ts_full_frame); /* Offset for next TS packet */
			/* Copy first frame directly into buffer */
			memcpy(aggregation->buffer, pes_ptr + start_of_pes + TS_PACKET_POINTER(ts_full_frame), TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes - TS_PACKET_POINTER(ts_full_frame));
			/* Packet is not finished yet, it cannot be handled now */
	#ifdef DEBUG
			fprintf (stderr, "%s: Found multi-frame-table 0x%2.2x (data in first packet %d), Section-Length: %d, Section: %d, payload_start: %d (Pointer: %d)\n",
				channel_name(type), EIT_PACKET_TABLEID((start + TS_PACKET_POINTER(ts_full_frame))),
				TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes - TS_PACKET_POINTER(ts_full_frame),
				EIT_SECTION_LENGTH((start + TS_PACKET_POINTER(ts_full_frame))), EIT_SECTION_NUMBER((start+ TS_PACKET_POINTER(ts_full_frame))),
				TS_PACKET_PAYLOAD_START(ts_full_frame), TS_PACKET_POINTER(ts_full_frame));
			DumpHex(start + TS_PACKET_POINTER(ts_full_frame), TS_PACKET_SIZE - TS_HEADER_SIZE - start_of_pes - TS_PACKET_POINTER(ts_full_frame));
	#endif
			return 0;
		}
		return 1;
	} else {
		return 1;
	}
	return 0;
}

/* For normal people SDT contains the Service description, i.e. the Station name. Some satellite stations are transmitting garbage
 * and remove the data from SDT and use PMT or whatever for it.
 * Sometimes it is just missing or removed by broken MPEG software */

static void extract_sdt_payload(unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes, unsigned char* ts_full_frame) {
	unsigned char* start = NULL;
	start = pes_ptr + start_of_pes;
	/* collect up continuation frames */
	if (! collect_continuation(sdt_table, pes_ptr, pes_len, start_of_pes, ts_full_frame, CHANNEL_TYPE_SDT)) {
		return;
	}
	/* SDT can use most of the PMT stuff */
	if (! sdt_table->buffer_valid) {
		return;
	}
	start = sdt_table->buffer;
#ifdef DEBUG
	fprintf (stderr, "SDT: Found data, table 0x%2.2x (Section length %d), program number %d, section %d, last section %d\n",
		PMT_TABLE_ID(start),
		PMT_SECTION_LENGTH(start),
		PMT_PROGRAM_NUMBER(start),	
		PMT_SECTION_NUMBER(start),
		PMT_LAST_SECTION_NUMBER(start));
	DumpHex(start, 183);
#endif
	unsigned char * description_offset = SDT_FIRST_DESCRIPTOR(start);
#ifdef DEBUG
	if (1) {
#else
	if (global_state->sdt_fromstream == 0) {
#endif
		if (crc32(start, sdt_table->section_length + 3) != 0) {
		#ifdef DEBUG
			output_logmessage("SDT: crc32 does not match, calculated %d, expected 0\n", crc32(start, sdt_table->section_length + 3));
		#endif
		} else {
			if ( PMT_TABLE_ID(start) == 0x42 ) {
				/* Table 0x42 contains information about current stream, we only want programm "running" (see mpeg standard for this hardcoded stuff) */
				while (description_offset < (SDT_FIRST_DESCRIPTOR(start) +  PMT_SECTION_LENGTH(start))) {
					if(PMT_PROGRAM_NUMBER(start) != global_state->transport_stream_id) {
#ifdef DEBUG
						fprintf(stderr, "SDT: SDT programm-number %d doesn't match global transport_stream_id %d\n", PMT_PROGRAM_NUMBER(start), global_state->transport_stream_id);
#endif
#if 0
					// I thought this is correct, but it doesn't work for the new french tranponder 12285V on Astra 19.2
					if(SDT_DESCRIPTOR_SERVICE_ID(description_offset) != global_state->service_id) {
#ifdef DEBUG
						fprintf(stderr, "SDT: SDT service %d doesn't match global service id (%d), transport_stream_id %d, programm_id %d\n",
							SDT_DESCRIPTOR_SERVICE_ID(description_offset), global_state->service_id,
							global_state->transport_stream_id, -1);
						// DumpHex(description_offset, 188);
						// fprintf(stderr, "----------------\n");
#endif
#endif
					} else {
						if ( SDT_DESCRIPTOR_SERVICE_ID(description_offset) != global_state->service_id ) {
#ifdef DEBUG
							fprintf(stderr, "SDT: SDT skipping service %d, because not matching for wanted sevice_id %d\n", 
								SDT_DESCRIPTOR_SERVICE_ID(description_offset), global_state->service_id );
#endif
							description_offset = description_offset + SDT_DESCRIPTOR_LOOP_LENGTH(description_offset) + 5;
							continue;
						}
						if (SDT_DESCRIPTOR_RUNNING(description_offset) == 0x4 ||
							SDT_DESCRIPTOR_RUNNING(description_offset) == 0x1 /* FRENCH transponder?! */ ) {
							unsigned char* description_content = SDT_DESCRIPTOR_CONTENT(description_offset);
							char provider_name[STR_BUF_SIZE];
							char service_name[STR_BUF_SIZE];
							uint8_t service_name_length = description_content[SDT_DC_PROVIDER_NAME_LENGTH(description_content) + 4];
							unsigned char * tmp = description_content + SDT_DC_PROVIDER_NAME_LENGTH(description_content) + 5;
							if (SDT_DC_PROVIDER_NAME_LENGTH(description_content) < STR_BUF_SIZE) {
								if (SDT_DC_PROVIDER_NAME(description_content)[0] < 0x20) {
									/* MPEG Standard has very sophisticated charset encoding, therefore a simple Hack for my setup */
									snprintf(provider_name, SDT_DC_PROVIDER_NAME_LENGTH(description_content), "%s", SDT_DC_PROVIDER_NAME(description_content + 1) );
								} else {
									snprintf(provider_name, SDT_DC_PROVIDER_NAME_LENGTH(description_content) + 1, "%s", SDT_DC_PROVIDER_NAME(description_content));
								}
							}
							/* like above, but written compacted, if first character is smaller 0x20 it's the charset encoding */
							snprintf(service_name, service_name_length + (tmp[0]< 0x20?0:1), "%s", tmp + (tmp[0]< 0x20?1:0));
							/* Service 0x02, 0x0A, 0x07: (Digital) Radio */
							if (SDT_DC_SERVICE_TYPE(description_content) == 0x2
								|| SDT_DC_SERVICE_TYPE(description_content) == 0x0a
								|| SDT_DC_SERVICE_TYPE(description_content) == 0x07
								|| SDT_DC_SERVICE_TYPE(description_content) == 0x01 /* broken station has SD-TV set */ ) {
								/* Sometime we get garbage only store if we have a service_name with length > 0 */
								if (strlen(service_name) > 0) {
									/* service name is latin1 in most cases */
									unsigned char utf8_service_name[STR_BUF_SIZE];
									utf8((unsigned char*)service_name, utf8_service_name);
									/* Yes, we want to get information about the programme */
									output_logmessage("SDT: Stream is station %s from network %s.\n", utf8_service_name, provider_name);
									strncpy(global_state->station_name, service_name, STR_BUF_SIZE);
									global_state->sdt_fromstream = 1;
									break; /* leave while loop */
								}
							} else {
								/* If service type is 0xff it's very likely just a stuffing frame without any content */
								unsigned char utf8_service_name[STR_BUF_SIZE];
								if (strlen(service_name) > 0) {
									unsigned char utf8_service_name[STR_BUF_SIZE];
									utf8((unsigned char*)service_name, utf8_service_name);
								}
								if (SDT_DC_SERVICE_TYPE(description_content) == 0x1f) {
									output_logmessage("SDT: Warning: Stream (also) contains service HEVC HDTV (%s)\n", utf8_service_name); 
								} else if (SDT_DC_SERVICE_TYPE(description_content) != 0xff) {
									output_logmessage("SDT: Warning: Stream (also) contains unkown service with id 0x%2x (%s)\n", SDT_DC_SERVICE_TYPE(description_content), utf8_service_name);
								}
							}
						}
					}
					description_offset = description_offset + SDT_DESCRIPTOR_LOOP_LENGTH(description_offset) + 5;
				} /* end while */
			}
		}
	}
	sdt_table->buffer_valid = 0;
	return;
}


static void extract_eit_payload(unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes, unsigned char* ts_full_frame )
{
	if (global_state->found_rds > 0) {
		return;
	}
	unsigned char* start = NULL;
	//uint8_t buffer[EIT_BUF_SIZE];
	//uint8_t size = 0;
	char short_description[STR_BUF_SIZE];
	char text_description[STR_BUF_SIZE];

	memset(short_description, 0, STR_BUF_SIZE);
	memset(text_description, 0, STR_BUF_SIZE);
	
	start = pes_ptr + start_of_pes;

	/* collect up continuation frames */
	if (! collect_continuation(eit_table, pes_ptr, pes_len, start_of_pes, ts_full_frame, CHANNEL_TYPE_EIT)) {
		return;
	}
	start = eit_table->buffer;
	if (eit_table->buffer_valid) {
		#ifdef DEBUG
			fprintf(stderr, "EIT: crc32 %s (%d, l: %d)\n",(  crc32(start, EIT_SECTION_LENGTH(start)+3)== 0?"OK":"FAIL"), crc32(start, EIT_SECTION_LENGTH(start)+3), EIT_SECTION_LENGTH(start)+3);
			DumpHex(start, EIT_SECTION_LENGTH(start));
		#endif
	}
	if (crc32(start, EIT_SECTION_LENGTH(start)+3)!= 0) {
		/* crc32 not valid, throw away continued data */
		eit_table->ob_used = 0;
		eit_table->buffer_valid = 0;
		return; 	
	}
	/* 0x4e current_event table */
	if (eit_table->buffer_valid == 1 &&  0x4e == EIT_PACKET_TABLEID(start)) {
		/* Current programme found */
		unsigned char* event_start = EIT_PACKET_EVENTSP(start);
		unsigned char* description_start = EIT_EVENT_DESCRIPTORP(event_start);
		unsigned int service_id = EIT_SERVICE_ID(start);
		if ( service_id != global_state->service_id ) {
#ifdef DEBUG
			fprintf(stderr, "EIT: service_id %d is wrong (%d)\n", service_id, global_state->service_id);
#endif
			eit_table->buffer_valid = 0;
			return;
		}
#ifdef DEBUG
		unsigned char* first_description_start = EIT_EVENT_DESCRIPTORP(event_start);
		fprintf(stderr, "EIT: Found event with id %d, currently in status %d, starttime %6.6x, duration %6.6x, Section: %d (V:%d) from %d (Length: %d).\n",
			EIT_EVENT_EVENTID(event_start),
			EIT_EVENT_RUNNING_STATUS(event_start),
			EIT_EVENT_STARTTIME_TIME(event_start),
			EIT_EVENT_DURATION(event_start),
			EIT_SECTION_NUMBER(start),
			EIT_VERSION_NUMBER(start),
			EIT_LAST_SECTION_NUMBER(start),
			EIT_SECTION_LENGTH(start));
#endif
		/* 0x4d = Short event descriptor found, transport_stream matches */
		if (DESCRIPTOR_TAG(description_start) == 0x4d && EIT_TRANSPORT_STREAM_ID(start) == global_state->transport_stream_id ) {
			/* Now calculate crc32, because we want to do something with the data */
			if (crc32(start, EIT_SECTION_LENGTH(start)+3) != 0) {
				#ifdef DEBUG
				fprintf(stderr, "EIT: crc32 does not match, calculated %d, expected 0, using section length: %d\n", crc32(start, EIT_SECTION_LENGTH(start)+3), EIT_SECTION_LENGTH(start)+3);
				#endif
				eit_table->buffer_valid = 0;
				return;
			}
			//fprintf(stderr, "EIT: Dumping full buffer .. \n");
			// write(2, start, eit_table->section_length);
			// fprintf(stderr, "\n");
			/* Step through the event descriptions */
			uint16_t current_in_position = 0;
			uint16_t current_out_position = 0;
			uint16_t max_size = EIT_EVENT_LOOPLENGTH(event_start);
			unsigned int stringlen = 0;
			unsigned char* text1_start = NULL;
			enum_charset text_charset; 
			int text1_len = 0;
			uint8_t * tmp = EIT_NAME_CONTENT(description_start);
			/* The same charset issue as with the SDT */
			stringlen = EIT_NAME_LENGTH(description_start) + (tmp[0] < 0x20? 0 : 1);
			snprintf(short_description, stringlen, "%s", EIT_NAME_CONTENT(description_start) + (tmp[0] < 0x20? 1 : 0) );
			if (tmp[0] < 0x20) {
				text_charset = tmp[0];
			} else {
				text_charset = CHARSET_LATIN1;
			}
			while (current_in_position + 60 <= max_size && current_in_position < eit_table->section_length) {
				stringlen = EIT_NAME_LENGTH(description_start);
				text1_start = description_start + EIT_SIZE_DESCRIPTOR_HEADER + stringlen;
				text1_len = text1_start[0];
				if (text1_len == 0 || text1_len > max_size) break;
				/* Avoid the character code marker 0x05 if it's a latin1 text */
				memcpy(text_description + current_out_position, text1_start + (text1_start[1] < 0x20 ? 2 : 1), text1_len - ((text1_start[1] < 0x20) ? 1 : 1));
				/* First round? */
				if (current_out_position == 0 && ( 70 < max_size) ) {
					/* Hack, if the overall text is very short, no "~" */
					strcpy(text_description + current_out_position + text1_len - 1, " ~ ");
					memset(text_description + current_out_position + text1_len + 3 - 1, 0, 1);
					current_out_position += text1_len + 3 - 1;
				} else {
					memset(text_description + current_out_position + text1_len - 1, 0, 1);
					current_out_position += text1_len - 1;
				}
#ifdef DEBUG
				fprintf(stderr, "DEBUG: text1 (charset: 0x%x): %s, in_pos: %d (%d), text1_len: %d, global_len: %d\n", text1_start[1], text_description, current_in_position,
					current_in_position + text1_len + EIT_SIZE_DESCRIPTOR_HEADER + stringlen, text1_len, max_size);
#endif
				description_start = description_start + EIT_SIZE_DESCRIPTOR_HEADER + stringlen + 1 + text1_len + 1;
				current_in_position += EIT_SIZE_DESCRIPTOR_HEADER + stringlen + 1 + text1_len + 1;
			}
			/* Replace control characters */
			cleanup_mpeg_string(text_description);
			if ( EIT_EVENT_RUNNING_STATUS(event_start) == 4) {
				char tmp_title[STR_BUF_SIZE+1];
#ifdef DEBUG				
				fprintf(stderr, "EIT: RUNNING EVENT: %s (%s) in language %3.3s found.\n", short_description, text_description, EIT_DESCRIPTOR_LANG(first_description_start));
#endif			/* Write full info into channel title, but only if there is a difference between text and short description */
				if (text_description != NULL && strlen(text_description) > 0 ) {
					int retval;
					retval = snprintf(tmp_title, STR_BUF_SIZE, "%s - %s", short_description, text_description);
					assert(retval > 0);
				} else {
					/* Sonst nur short description */
					int retval;
					retval = snprintf(tmp_title, STR_BUF_SIZE, "%s", short_description);
					assert(retval > 0);
				}
				if (0 != strcmp(tmp_title, global_state->stream_title)) {
					// It's needed in iso8859-1 for StreamTitle, but in UTF-8 for logging
					unsigned char utf8_message[STR_BUF_SIZE];
					strcpy(global_state->stream_title, tmp_title);
					if (text_charset == CHARSET_LATIN1) {
						output_logmessage("EIT: Current transmission `%s'\n", utf8((unsigned char*)short_description, utf8_message));
					} else if ( text_charset == CHARSET_UTF8 ) {
						output_logmessage("EIT: Current transmission `%s'\n", short_description); 
					} else {
						output_logmessage("EIT: Current transmission (MPEG-Charset: 0x%x, output likely garbaled) `%s'\n", text_charset, utf8((unsigned char*)short_description, utf8_message));
					}
				}
			} else {
#ifdef DEBUG
				fprintf(stderr, "EIT: Event %s in Language %3.3s found.\n", short_description, EIT_DESCRIPTOR_LANG(first_description_start));
#endif
			}
		}
	}
	eit_table->buffer_valid = 0;
	return;
}


int32_t extract_pes_payload( unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes )
{
	unsigned char* es_ptr=NULL;
	size_t es_len=0;

	int32_t bytes_written = 0;
	// static uint32_t pes_framesize = 0; 

	/* Start of audio block / PES? */
	if ( start_of_pes ) {
		/* Parse and remove PES header */
		es_ptr = parse_pes( pes_ptr, pes_len, &es_len, chan );
#if 0
		fprintf(stderr, "extract_pes_payload new frame: Frame#%lu, chan->pes_remaining = %ld\n", frame_count, chan->pes_remaining);
#endif

	} else if (chan->pes_stream_id) {
		// Don't output any data until we have seen a PES header
		es_ptr = pes_ptr;
		es_len = pes_len;
		// Are we are the end of the PES packet?
		if (es_len>chan->pes_remaining) {
			output_logmessage("extract_pes_payload: Frame#%lu chan->pes_remaining (%ld) < es_len (%d)!\n", frame_count, chan->pes_remaining, es_len);
			es_len=chan->pes_remaining;
		}
	}
	// Subtract the amount remaining in current PES packet
	chan->pes_remaining -= es_len;
#if 0 
		fprintf(stderr, "extract_pes_payload after substraction of current frame: Frame#%lu, chan->pes_remaining = %ld\n", frame_count, chan->pes_remaining);
#endif
	// Got some data to write out?
	if (es_ptr) {
		// Scan through Elementary Stream (ES)
		// and try and find MPEG audio stream header
		while (!chan->synced && es_len>= 6) {
			// Valid header?
			// MPEG1/2
			// fprintf(stderr, "Searching for stream start, Offset %d, Pointer %d, Bytes 0x%x, 0x%x\n", es_len, es_ptr, es_ptr[0], es_ptr[1]); 

			if (chan->pes_stream_id >= 0xc0) {
				if (mpa_header_parse(es_ptr, &chan->mpah)) {
					// Now we know bitrate etc, set things up
					// output_logmessage("Synced to MP1/MP2 audio in PID %d stream: 0x%x\n", chan->pid, chan->pes_stream_id );
					mpa_header_print( &chan->mpah );
					chan->synced = 1;
					chan->payload_size = 2048;
				}
			} else {
				if (ac3_header_parse(es_ptr, &chan->mpah)) {
					// output_logmessage("Synced to AC-3 audio in PID %d stream: 0x%x\n", chan->pid, chan->pes_stream_id );
					ac3_header_print( &chan->mpah );
					chan->synced = 1;
					chan->payload_size = 2048;
				}
			}
			if (chan->synced) {
				// Allocate buffer to store packet in
				chan->buf_size = chan->payload_size + TS_PACKET_SIZE;
				chan->buf = realloc( chan->buf, chan->buf_size + 4 );
				if (chan->buf==NULL) {
					output_logmessage("Error: Failed to allocate memory for MPEG Audio buffer\n");
					exit(-1);
				}
				chan->buf_ptr = chan->buf;
				// Initialise the RTP TS to the PES TS
			} else {
				// Skip byte
				es_len--;
				es_ptr++;
			}
		}
		// If stream is synced then put data info buffer
		if (chan->synced && global_state->output_payload) {
			// Check that there is space
			if (chan->buf_used + es_len > chan->buf_size) {
				output_logmessage("Error: MPEG Audio buffer overflow\n" );
				exit(-1);
			}
			// Copy data into the buffer
			memcpy( chan->buf_ptr + chan->buf_used, es_ptr, es_len);
			chan->buf_used += es_len;
		}
	}
	/* Okay, actually this doesn't fit very well here, but we want to update the
	 * cache only if the payload channel is in sync, and we have the SDT ready.
	 * we have easy access to the sync data here, therefore we access it
	 * here - Perhaps we can move this elsewhere TODO */
	if ((!global_state->cache_written) &&
		cgi_mode					   &&
		global_state->sdt_fromstream   &&
		chan->synced) {
		add_cache(global_state);
		global_state->cache_written = 1;
	}
	// every time the buffer is full scan for RDS data. Hopefully we get the RDS data
	if (chan->buf_used > chan->payload_size && global_state->output_payload) {
		rds_data_scan(chan);
	}
	// Got enough to send packet and we are allowed to output data
	if (chan->buf_used > chan->payload_size && global_state->output_payload ) {
		#ifndef DEBUG
		/* If Icy-MetaData is set to 1 a shoutcast StreamTitle is required all 8192 */
		/* (SHOUTCAST_METAINT) Bytes */
		/* see documentation: https://cast.readme.io/docs/icy */
		if (shoutcast) {
			if (chan->payload_size + chan->bytes_written_nt <= SHOUTCAST_METAINT) {
				if (chan->payload_size > 0) {
					if (! fwrite((char*)chan->buf, chan->payload_size, 1, stdout) ) {
						if ( ferror(stdout)) {
							output_logmessage("write_streamdata: Error during write: %s, Exiting.\n", strerror(errno));
							/* Not ready */
							return -1;
						} else {
							output_logmessage("write_streamdata: EOF on STDOUT(?) during write.\n");
							return -1;
						}
					}
					chan->bytes_written_nt += chan->payload_size;
					bytes_written += chan->payload_size;
				}
			} else {
				uint32_t first_write = SHOUTCAST_METAINT - chan->bytes_written_nt;
				uint32_t second_write = chan->payload_size - first_write;
				uint16_t bytes = 0;
				uint16_t written = 0;
				char streamtitle[STR_BUF_SIZE];
				/* Only output StreamTitle if it's different or for the first time */
				if (strcmp(global_state->stream_title, global_state->old_stream_title) != 0) {
					/* Maximum of 2000 characters! */
					snprintf(streamtitle, STR_BUF_SIZE - 1, "StreamTitle='%.2000s';", global_state->stream_title);
					strcpy(global_state->old_stream_title, global_state->stream_title);
				} else {
					memset(streamtitle, 0, strlen(global_state->stream_title) + 1);
				}
				bytes = ((strlen(streamtitle))>>4) + (strlen(streamtitle) > 0?1:0);
				/* Shift right by 4 bit => Divide by 16, and add 1 to get minimum possible length.
				 * Add 1 only, if size is > 0, otherwise there is no metadata and therefore nothing to send */
				if (first_write > 0) {
					if (! fwrite((char*)chan->buf, first_write, 1, stdout)) {
						if (ferror(stdout)) {
							output_logmessage("write_streamdata: Error during write: %s, Exiting.\n", strerror(errno));
						} else {
							output_logmessage("write_streamdata: Error or EOF on STDOUT(?) during write.\n");
						}
						return -1;
					}
					bytes_written += first_write;
					chan->bytes_written_nt += first_write;
				}
				fwrite(&bytes, 1, 1, stdout);
				fwrite(streamtitle, bytes<<4, 1, stdout);
				bytes_written += 1;
				bytes_written += bytes<<4;
				if (second_write > 0) {
					if (! fwrite((char*)(chan->buf + first_write), second_write, 1, stdout) ) {
						if (ferror(stdout)) {
							output_logmessage("write_streamdata: Error during write: %s, Exiting.\n", strerror(errno));
						} else {
							output_logmessage("write_streamdata: Error or EOF on STDOUT(?) during write.\n");
						}
						return -1;
					}
					written = second_write;
					bytes_written += written;	
					/* Reset the Shoutcastcounter */
					chan->bytes_written_nt = second_write;
				}
				fflush(stdout);
			}
		} else {
			if (chan->payload_size > 0 && ! fwrite((char*)chan->buf, chan->payload_size, 1, stdout) ) {
				output_logmessage("write_streamdata: Error or EOF on STDOUT(?) during write.\n");
				return -1;
			}
			bytes_written += chan->payload_size;
			chan->bytes_written_nt += chan->payload_size;
		}
		#endif
		// Move any remaining memory to the start of the buffer
		chan->buf_used -= chan->payload_size;
		memmove( chan->buf_ptr, chan->buf_ptr+chan->payload_size, chan->buf_used );
		
	}
	return bytes_written;
}

/* In FILTER mode (non-cgi-mode) we simply start with a file descriptor. This is a
 * leftover from the original code, because in our case it is always stdin
 * This is the main processing loop for filter mode that runs until we have no
 * longer data on stdin (read returns 0) or we caught a signal (Interrupted > 0) */

void filter_global_loop(int fd_dvr) {
	unsigned char buf[TS_PACKET_SIZE];
	int bytes_read;
	const uint16_t max_sync_errors = 5;
	
	while (! Interrupted ) {
		bytes_read = read(fd_dvr, buf, TS_PACKET_SIZE);
		global_state->bytes_streamed_read += bytes_read;
		if (bytes_read == 0) {
			output_logmessage("filter_global_loop: read from stream %.2f MB, wrote %.2f MB, no bytes left to read - EOF. Exiting.\n",
				(float)global_state->bytes_streamed_read/mb_conversion, (float)global_state->bytes_streamed_write/mb_conversion);
			break;
		}
		if (bytes_read > 0 && bytes_read < TS_PACKET_SIZE) {
			output_logmessage("filter_global_loop: short read, only got %d bytes, instead of %d, trying to resync\n", bytes_read, TS_PACKET_SIZE);
			// trying to start over, to get a full read by waiting a little bit (450 ms)
			poll(NULL, 0, 450);
			/* Throw rest of the not read bytes away by trying to read it in the buffer and jump back to read full mpeg frames */
			bytes_read = read(fd_dvr,buf,TS_PACKET_SIZE - bytes_read);
			global_state->ts_sync_error += 1;
			if (global_state->ts_sync_error > max_sync_errors) {
				break;
			}
			continue;
		} else if (bytes_read < 0) {
			output_logmessage("filter_global_loop: streamed %ld bytes, read returned an error: %s, exiting.\n", global_state->bytes_streamed_read, strerror(errno));
			break;
		}
		// Check the sync-byte
		if (TS_PACKET_SYNC_BYTE(buf) != 0x47) {
			int i = 1;
			int did_read = 0;
			/* Check whether we are completly lost
			 * (got no synchronisation on transport-stream start after max_sync_errors tries) */
			global_state->ts_sync_error += 1;
			if (global_state->ts_sync_error > max_sync_errors) {
				output_logmessage("filter_global_loop: After reading %.2f MB and writing %.2f, " \
				                  "Lost synchronisation (sync loss counter of %d exceeded) - Exiting\n",
					(float)global_state->bytes_streamed_read/mb_conversion, (float)global_state->bytes_streamed_write/mb_conversion, max_sync_errors);
				return;
			}
			for (i = 1; i < TS_PACKET_SIZE; i++) {
				if (0x47 == buf[i]) {
					/* throw rest of buffer away */
					bytes_read = read(fd_dvr, buf, i);
					did_read = i;
					continue;
				}
			}
			/* No 0x47 found, most likly we are lost */
			if (0 == did_read) {
				output_logmessage("filter_global_loop: After reading %.2f MB and writing %.2f, " \
				                  "Lost synchronisation - skipping full block (Lost counter %d, aborting at %d) \n",
					(float)global_state->bytes_streamed_read/mb_conversion, (float)global_state->bytes_streamed_write/mb_conversion,
					global_state->ts_sync_error, max_sync_errors);
			} else {
				output_logmessage("filter_global_loop: After reading %.2f MB and writing %.2f, " \
				                  " Lost synchronisation - skipping %d bytes (Lost counter %d, aborting at %d) \n",
					(float)global_state->bytes_streamed_read/mb_conversion, (float)global_state->bytes_streamed_write/mb_conversion,
					did_read, global_state->ts_sync_error, max_sync_errors);
			}
			continue; /* read next block */
		}
		/* Bail out on hard errors */
		if (process_ts_packet(buf) == TS_HARD_ERROR) {
			break;
		}
	}
	return;
}

/* In CGI mode we are called by libcurl using the libcurl CURLOPT_WRITEFUNCTION callback function
 * we don't know how much data we get at once, but we've to handle it completly before going back.
 * This is no big deal because this code is much faster then needed for processing */

struct memory_struct {
	unsigned char *memory;
	size_t size;
};

/* libcurls write callback, set with CURLOPT_WRITEFUNCTION if in CGI mode */
/* the code is inspired by an example implementation given on libcurls webpage */

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	size_t already_processed = 0;
	char header[STR_BUF_SIZE];
	struct memory_struct *mem = (struct memory_struct *)userp;

	/* process the data we've stored from the last run? */
	unsigned char * buf = contents;

	/* Do we have to output the HTTP Header? */
	if (!global_state->output_payload) {
		/* not all data items were available, check wether they are available now.
		 * output the header and START playing audio */
		if (global_state->station_name
			&& strlen(global_state->station_name) > 0
			&& global_state->br > 0
			&& global_state->sr > 0) {
			if (shoutcast) {
				/* Strlen: of all the static stuff: 114 Byte */
				snprintf(header, STR_BUF_SIZE, "Content-Type: %s\n" \
						"Connection: close\n" \
						"icy-br: %d\n" \
						"icy-sr: %d\n" \
						"icy-name: %.120s\n" \
						"icy-metaint: %d\n\n",
						global_state->mime_type,
						global_state->br * 1000, global_state->sr, global_state->station_name, SHOUTCAST_METAINT);
			} else {
				snprintf(header, STR_BUF_SIZE, "Content-Type: %s\n" \
						"Connection: close\n\n", global_state->mime_type);
			}
			fwrite(header, strlen(header), 1, stdout);
			fflush(stdout);
			global_state->output_payload = 1;
		}
	}
	
	/* Do we have data from the last run that we must use for the next one? */
	if (mem->size > 0) {
		/* We have a remaining leftover from last curl call. Make a call with a filled up buffer first */
		memcpy(mem->memory + (mem->size), buf, TS_PACKET_SIZE - mem->size);
		already_processed = TS_PACKET_SIZE - mem->size;
		global_state->bytes_streamed_read += TS_PACKET_SIZE - mem->size;
		buf += TS_PACKET_SIZE - mem->size;
		if (TS_PACKET_SYNC_BYTE(mem->memory) == 0x47) {
			if (process_ts_packet(mem->memory) == TS_HARD_ERROR) {
				already_processed = 0;
				goto write_error;
			}
		} else {
			output_logmessage("write_callback (curl): got short block, skipped data at block position %d (%d)\n", 0, global_state->bytes_streamed_read);
			/* TODO resync? */
		}		
		mem->size = 0;
	}
	while (already_processed < realsize && already_processed + TS_PACKET_SIZE <= realsize) {
		global_state->bytes_streamed_read += TS_PACKET_SIZE;
		if (TS_PACKET_SYNC_BYTE(buf) == 0x47) {
			if ( process_ts_packet(buf) != TS_HARD_ERROR ) {
				already_processed += TS_PACKET_SIZE;
				buf += TS_PACKET_SIZE;
			} else {
				/* an error occured */
				already_processed = 0;
				goto write_error;
			}
		} else {
			/* TODO resync? */
			output_logmessage("write_callback (curl): Skipped data at block position %d, realsize %d (%.2f MB read)\n", already_processed, realsize, (float)global_state->bytes_streamed_read/mb_conversion);
			already_processed += TS_PACKET_SIZE;
			buf += TS_PACKET_SIZE;
		}
	}
	/* in some cases curl fetches an "non-even amount" of mpeg-ts data, means we get some remaining data
	 * we save it here for later processing. "already_processed != real_size" is the special case where a remainder of
	 * a 2nd read is exactly the right size.
	 * (e.g. libcurl reads the maximum of 16384 byte, remaining 28 byte, next read 160 byte == 188 byte == full mpeg ts frame */
	if (already_processed > 0 && already_processed < realsize && already_processed + TS_PACKET_SIZE > realsize && ( already_processed != realsize) ) {
		// output_logmessage("DEBUG: uneven amount remaining: %d bytes (curl called us with %d bytes)\n", realsize - already_processed, realsize);
		memcpy(mem->memory, buf, realsize - already_processed);
		mem->size = realsize - already_processed;
		already_processed += realsize - already_processed;
		global_state->bytes_streamed_read += realsize - already_processed;
	}
	if (Interrupted) {
write_error:
		already_processed = 0;
	}
	return already_processed;
}

void start_curl_download() {

	/* exactly one full ts frame can be stored */
	struct memory_struct chunk;
	chunk.size = 0;
	chunk.memory = calloc(1, TS_PACKET_SIZE);
	char user_agent_string[STR_BUF_SIZE];
	char url[STR_BUF_SIZE];

	curl_global_init(CURL_GLOBAL_ALL);
	CURL *curl = curl_easy_init();
	if (! curl) {
		output_logmessage("Cannot initialize libcurl at all, exiting\n");
		exit(1);
	}
	/* prepare headers for accessing tvheadend */
	struct curl_slist *header = NULL;
	/* We only want mpeg transport */
	header = curl_slist_append(header, "Accept: audio/mp2t");
	/* Limit headers to a length that makes sense */
	if (getenv("HTTP_USER_AGENT")) {
		snprintf(user_agent_string, STR_BUF_SIZE, "User-Agent: ts2shout for %.200s", getenv("HTTP_USER_AGENT"));
	} else {
		snprintf(user_agent_string, STR_BUF_SIZE, "User-Agent: ts2shout");
	}
	if (getenv("REMOTE_ADDR")) {
		char s[STR_BUF_SIZE];
		snprintf(s, STR_BUF_SIZE, "Forwarded: by \"%s\"; for \"%.200s\"; proto=http", "127.0.0.1", getenv("REMOTE_ADDR"));
		header = curl_slist_append(header, s);
	}
	header = curl_slist_append(header, user_agent_string);
	int res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header);
	if (getenv("REDIRECT_PROGRAMMNO")) {
		global_state->programme = getenv("REDIRECT_PROGRAMMNO");
	} else {
		global_state->programme = getenv("PROGRAMMNO");
	}
	char *escaped_programmno = curl_easy_escape(curl, global_state->programme, 0);
	if (! escaped_programmno) {
		output_logmessage("curl_easy_escape() on %s failed! Aborting\n", global_state->programme);
		exit(1);
	}
	/* generate URI (TVHEADEND and PROGRAMMNO was checked by calling function) */
	if (getenv("REDIRECT_TVHEADEND")) {
		if (strncmp(getenv("REDIRECT_TVHEADEND"), "http://", 7) == 0) {
			snprintf(url, STR_BUF_SIZE, "%s/%s", getenv("REDIRECT_TVHEADEND"), escaped_programmno);
		} else {
			snprintf(url, STR_BUF_SIZE, "http://%s/%s", getenv("REDIRECT_TVHEADEND"), escaped_programmno);
		}
	} else {
		if (strncmp(getenv("TVHEADEND"), "http://", 7) == 0) {
			snprintf(url, STR_BUF_SIZE, "%s/%s", getenv("TVHEADEND"), escaped_programmno);
		} else {
			snprintf(url, STR_BUF_SIZE, "http://%s/%s", getenv("TVHEADEND"), escaped_programmno);
		}
	}
	if ( escaped_programmno) {
		curl_free(escaped_programmno);
	}
	/* Try to get cached parameters from last session */
	fetch_cached_parameters(global_state);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
	/* Set timeout to avoid hangin processes */
	/* abort if slower than 2000 bytes/sec during 5 seconds */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 2000L);
#ifdef DEBUG
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif
	res = curl_easy_perform(curl);
	// We almost always get the write error during our writes, because we tell
	// libcurl that we want to stop download by return "0" in the write callback
	// We don't need the errors in the log
	if( (res != CURLE_OK) && (res != CURLE_WRITE_ERROR) ) {
		output_logmessage("curl_easy_perform() failed on %s: %s\n",
			url, curl_easy_strerror(res));
	} else {
		/* Do something */
	}
	output_logmessage("curl_download: %s after fetching %.2f MB and writing %.2f MB. Exiting.\n",
		((Interrupted >0 && Interrupted <=32)?strsignal(Interrupted):"streaming error"),
		(float)global_state->bytes_streamed_read/mb_conversion, (float)global_state->bytes_streamed_write/mb_conversion );
	/* cleanup curl stuff */
	curl_easy_cleanup(curl);
	free(chunk.memory);
	curl_global_cleanup();
	return;
}


/* This function handles exactly one MPEG TS full frame of 188 bytes. It has to be checked before calling
 * whether a full frame of 188 byte has been received. process_ts_packet has to be called subsequently
 * with every frame, otherwise you'll get an out-of-sync / ts_continuity error */

int16_t process_ts_packet( unsigned char * buf )
{
	unsigned char* pes_ptr=NULL;
	unsigned int pid=0;
	size_t pes_len;
	int32_t streamed = 0;
	
	frame_count += 1 ;
/* DEBUG possibility
	fprintf(stderr, "ts-frame number #%d\n", frame_count); 
*/
	// Get the PID of this TS packet
	pid = TS_PACKET_PID(buf);
		
	// Transport error?
	if ( TS_PACKET_TRANS_ERROR(buf) ) {
		output_logmessage("process_ts_packet: Warning, transport error in PID %d.\n", pid);
		if (channel_map[ pid ]) {
			channel_map[ pid ]->synced = 0;
			channel_map[ pid ]->buf_used = 0;
		}
		return TS_SOFT_ERROR;
	}			

	// Scrambled? (Should never happen)
	if ( TS_PACKET_SCRAMBLING(buf) ) {
		output_logmessage("process_ts_packet: Warning: PID %d is scrambled.\n", pid);
		return TS_SOFT_ERROR;
	}

	// Location of and size of PES payload
	pes_ptr = &buf[4];
	pes_len = TS_PACKET_SIZE - 4;
	
	// Check for adaptation field?
	if (TS_PACKET_ADAPTATION(buf)==0x1) {
		// Payload only, no adaptation field
	} else if (TS_PACKET_ADAPTATION(buf)==0x2) {
		// Adaptation field only, no payload
		return TS_SOFT_ERROR;
	} else if (TS_PACKET_ADAPTATION(buf)==0x3) {
		// Adaptation field AND payload
#ifdef DEBUG
		output_logmessage("process_ts_packet: Adaption field with length %d found in frame #%d\n", TS_PACKET_ADAPT_LEN(buf), frame_count);
#endif 
		pes_ptr += (TS_PACKET_ADAPT_LEN(buf) + 1);
		pes_len -= (TS_PACKET_ADAPT_LEN(buf) + 1);
	}
	// Check we know about the payload
	if (channel_map[ pid ]) {
		// Continuity check
		ts_continuity_check( channel_map[ pid ], TS_PACKET_CONT_COUNT(buf) );
		enum_channel_type channel_type = channel_map[ pid ]->channel_type;
		global_state->ts_sync_error = 0;	/* Reset global ts_sync_error counter */
		switch (channel_type) {
			case CHANNEL_TYPE_PAT:
				extract_pat_payload(pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
				break;
			case CHANNEL_TYPE_EIT:
				extract_eit_payload ( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf), buf );
				// global_state->ts_sync_error = 0; /* no pid_change() because it resets the EIT stuff */
				break;
			case CHANNEL_TYPE_SDT:
				extract_sdt_payload ( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf), buf );
				break;
			case CHANNEL_TYPE_PMT:
				extract_pmt_payload ( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
				break;
			case CHANNEL_TYPE_PAYLOAD:
				streamed = extract_pes_payload( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
				if (streamed < 0) {
					/* cannot stream */
					return TS_HARD_ERROR;
				}
				global_state->bytes_streamed_write += streamed;
				break;
			default:
#ifdef DEBUG
				fprintf(stderr, "Warning: don't know anything about PID %d.\n", pid);
#endif
				break;
		}
	}
	return TS_PACKET_SIZE;
}

int main(int argc, char **argv)
{
	int fd_dvr=-1;
	int i;
	
	// Initialise data structures
	for (i=0;i<MAX_PID_COUNT;i++) channel_map[i]=NULL;
	for (i=0;i<MAX_CHANNEL_COUNT;i++) channels[i]=NULL;
	eit_table = calloc(1, sizeof(section_aggregate_t));
	sdt_table = calloc(1, sizeof(section_aggregate_t));
	global_state = calloc(1, sizeof(programm_info_t));
	
	/* Are we running as CGI programme? */
	if (getenv("QUERY_STRING")) {
		cgi_mode = 1;
		if (getenv("MetaData") && strncmp(getenv("MetaData"), "1", 1) == 0) {
			shoutcast = 1;
		} else if (getenv("REDIRECT_MetaData") && strncmp(getenv("REDIRECT_MetaData"), "1", 1) == 0) {
			shoutcast = 1;
		} else 	{
			shoutcast = 0;
		}
		if (getenv("AC3") && strncmp(getenv("AC3"), "1", 1) == 0) {
			global_state->want_ac3 = 1;
		}
		if (getenv("REDIRECT_AC3") && strncmp(getenv("REDIRECT_AC3"), "1", 1) == 0) {
			global_state->want_ac3 = 1;
		}
		if (getenv("RDS") && strncmp(getenv("RDS"), "1", 1) == 0) {
			global_state->prefer_rds = 1;
		}
		if (getenv("REDIRECT_RDS") && strncmp(getenv("REDIRECT_RDS"), "1", 1) == 0) {
			global_state->prefer_rds = 1;
		}
	} else {
		// Parse command line arguments
		parse_args( argc, argv );
	}
	init_structures();
	init_rds();
	output_logmessage("ts2shout version " XSTR(CURRENT_VERSION) " compiled " XSTR(CURRENT_DATE) " started\n");
	output_logmessage("%s %s in %s mode with%s RDS support.\n",
		(global_state->want_ac3?"AC-3 streaming":"MPEG streaming"),
		(shoutcast?"with shoutcast StreamTitles":"without shoutcast support, audio only"),
		(cgi_mode?"CGI":"FILTER"), (global_state->prefer_rds?"":"out") );
	// Setup signal handlers
	if (signal(SIGHUP, signal_handler) == SIG_IGN) signal(SIGHUP, SIG_IGN);
	if (signal(SIGINT, signal_handler) == SIG_IGN) signal(SIGINT, SIG_IGN);
	if (signal(SIGTERM, signal_handler) == SIG_IGN) signal(SIGTERM, SIG_IGN);

	if (! cgi_mode ) {
		// Open the DRV device
		if((fd_dvr = open("/dev/stdin", O_RDONLY)) < 0){
			perror("Failed to open STDIN device");
			return -1;
		}
		global_state->output_payload = 1;
		filter_global_loop( fd_dvr );
		if (Interrupted) {
				output_logmessage("Caught signal %d - closing cleanly.\n", Interrupted);
		}
	} else {
		/* In CGI mode */
		if (!getenv("REDIRECT_TVHEADEND") || ! getenv("REDIRECT_PROGRAMMNO")) {
			if (!getenv("TVHEADEND") || ! getenv("PROGRAMMNO") ) {
				output_logmessage("cgi_mode: Problems with environment, either REDIRECT_TVHEADEND / REDIRECT_PROGRAMMNO or TVHEADEND / PROGRAMMNO must be set. The following is the case: REDIRECT_TVHEADEND: %s, REDIRECT_PROGRAMMNO %s, TVHEADEND: %s, PROGRAMMNO: %s\n",
				getenv("REDIRECT_TVHEADEND"), getenv("REDIRECT_PROGRAMMNO"), getenv("TVHEADEND"), getenv("PROGRAMMNO"));
			} else {
				start_curl_download();
			}
		} else {
			start_curl_download();
		}
	}	
	// Clean up
	for (i=0;i<channel_count;i++) {
		if (channels[i]->buf) free( channels[i]->buf );
		free( channels[i] );
	}
	free(sdt_table);
	free(eit_table);
	if (! cgi_mode) {
		close(fd_dvr);
	}
	exit(0);
}

