/* 

	ts2shout.c
	(C) Carsten Gross <carsten@siski.de> 2018
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

#include "ts2shout.h"

int Interrupted=0;        /* Playing interrupted by signal? */
int channel_count=0;      /* Current listen channel count */

uint8_t shoutcast=1;      /* Send shoutcast headers? */
uint8_t	logformat=1;      /* Apache compatible output format */
uint8_t	cgi_mode=0;       /* Are we running as CGI programme? This is set if there is QUERY_STRING set in the environment */ 

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
	if (argc == 2) {
		if (strcmp("noshout", argv[1]) == 0) {
			shoutcast = 0;
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
	sprintf(current_time, current_time_fmt, (t.tv_nsec / 1000)); 
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
	if (chan->continuity_count==16)
		chan->continuity_count=0;
}


static void extract_pat_payload(unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes ) {
	unsigned char* start = NULL;
	start = pes_ptr + start_of_pes; 
#ifdef DEBUG
    fprintf (stderr, "PAT: Found data, table 0x%2.2d (Section length %d), transport_stream_id %d, section %d, last section %d\n",
		PAT_TABLE_ID(start),
		PAT_SECTION_LENGTH(start),
		PAT_TRANSPORT_STREAM_ID(start),	
		PAT_SECTION_NUMBER(start),
		PAT_LAST_SECTION_NUMBER(start));
#endif 
	unsigned char* programmes = PAT_PROGRAMME_START(start);
	/* Add PMT if not already there */
	if (! channel_map[PAT_PROGRAMME_PMT(programmes)] ) {
		if (crc32(start, PAT_SECTION_LENGTH(start) + 3) == 0) {
			add_channel(CHANNEL_TYPE_PMT, PAT_PROGRAMME_PMT(programmes));
			global_state->programm_id = PAT_TRANSPORT_STREAM_ID(start);
		} else {
			// fprintf (stderr, "PAT: crc32 does not match: calculated %d, expected 0\n", crc32(start, PAT_SECTION_LENGTH(start) + 3)); 
		}
			
	}
#ifdef DEBUG
	fprintf (stderr, "PAT: Programme 1, MAP-PID %d\n", PAT_PROGRAMME_PMT(programmes)); 
#endif
	
}

static void extract_pmt_payload(unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes ) {
	unsigned char* start = NULL;
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
		if (PMT_SECTION_NUMBER(start) == 0 && PMT_LAST_SECTION_NUMBER(start) == 0) {
			unsigned char* pmt_stream_info_offset = PMT_DESCRIPTOR(start);
			if (! channel_map[PMT_PID(pmt_stream_info_offset)]) {
				if (PMT_STREAM_TYPE(pmt_stream_info_offset) == 0x03 /* MPEG 1 audio */
					|| PMT_STREAM_TYPE(pmt_stream_info_offset) == 0x04 /* MPEG 2 audio */
					|| PMT_STREAM_TYPE(pmt_stream_info_offset) == 0x0f /* MPEG 2 audio */) {
				/* Everything matched, now we calculate the crc32 of the PMT frame, to avoid decoding garbage */
					if (crc32(start, PAT_SECTION_LENGTH(start) + 3) == 0) {	
						add_channel(CHANNEL_TYPE_PAYLOAD, PMT_PID(pmt_stream_info_offset));
					} else {
						output_logmessage("PMT: Cannot add audio-stream PID %d, PMT crc32 does not match: calculated %d, expected 0\n", 
							PMT_PID(pmt_stream_info_offset),
							crc32(start, PAT_SECTION_LENGTH(start) + 3));
					}
				} else {
#ifdef DEBUG
					fprintf(stderr, "PMT: Stream with pid %d found, but unsusable format (stream type %d)\n", 
			                PMT_PID(pmt_stream_info_offset), PMT_STREAM_TYPE(pmt_stream_info_offset));
#endif
				}
			}
		}
	}
}

uint8_t collect_continuation(section_aggregate_t* aggregation, unsigned char *pes_ptr, size_t pes_len, int start_of_pes, enum_channel_type type) {
	unsigned char* start = pes_ptr + start_of_pes;
	/* If an information block doesn't fit into an mpeg-ts frame it is continued in a next frame. The information is 
	 * directly attached after the PID (TS_HEADER_SIZE=4 Byte offset). It is possible that there is a multi-ts-frame continuation 
	 * we have to calculate a lot */
	if (aggregation->continuation > 0) {
#ifdef DEBUG
		fprintf(stderr, "%s: continued frame: offset: %d, counter: %d, section_length: %d\n", 
			channel_name(type), aggregation->offset, aggregation->counter, aggregation->section_length); 
		// fprintf(stderr, "EIT: Dumping currently received-stuff  .. \n");
		// write(2, start, TS_PACKET_SIZE - start_of_pes); 
		// fprintf(stderr, "\n");
#endif 
		memcpy(aggregation->buffer + aggregation->offset - TS_HEADER_SIZE, start, TS_PACKET_SIZE - start_of_pes);
		aggregation->offset += TS_PACKET_SIZE - TS_HEADER_SIZE; /* Offset for next TS packet */
		aggregation->counter += 1;
		if (aggregation->offset - TS_HEADER_SIZE >= aggregation->section_length) {
			/* Section is finished, finally */
			aggregation->buffer_valid = 1; 
			aggregation->continuation = 0; 
#ifdef DEBUG
			fprintf(stderr, "%s: finished multi-frame table after offset %d, counter: %d\n", 
				channel_name(type), aggregation->offset, aggregation->counter); 
#endif 
		} else {
			// This should not happen, because the standard recommends a maximum size of ~4K */
			if (aggregation->offset + TS_PACKET_SIZE - TS_HEADER_SIZE > EIT_BUF_SIZE ) {
				output_logmessage("collect_continuation: Maximum Data-Chunk Size of %d characters " \
				    "exceeded by MPEG-Transport-Stream. Did read %d continued packets\n", 
					EIT_BUF_SIZE, aggregation->counter);
				// reset internal buffer
				memset(aggregation, 0, sizeof(section_aggregate_t));
			}
			return 0;
		}
		/* Move start of packet to helper buffer for long frames */
	} 
	if ( aggregation->buffer_valid == 0) {
		/* A short EIT frame < TS_PACKET_SIZE length, take care of the crc32! */
		if ( aggregation->continuation == 0 && EIT_SECTION_LENGTH(start) < (TS_PACKET_SIZE - TS_HEADER_SIZE - 3 )   ) {
			aggregation->buffer_valid = 1; 
			aggregation->continuation = 0;
			aggregation->counter = 1; 
			aggregation->section_length = EIT_SECTION_LENGTH(start); 
			aggregation->offset = 0; 
			memcpy(aggregation->buffer, start, TS_PACKET_SIZE - start_of_pes );
	#ifdef DEBUG
			fprintf (stderr, "%s: Single frame-table 0x%2.2x (last table 0x%2.2x), Section-Length: %d (%d), Section: %d\n",
				channel_name(type), EIT_PACKET_TABLEID(start), EIT_PACKET_LAST_TABLEID(start), EIT_SECTION_LENGTH(start), TS_PACKET_SIZE - start_of_pes, EIT_SECTION_NUMBER(start)); 
	#endif 
		} else if ( ( EIT_SECTION_LENGTH(start) >= (TS_PACKET_SIZE - TS_HEADER_SIZE - 3)) && ( 0 == aggregation->continuation ) ) {
			/* A long frame, will be continued in next frame */
			aggregation->buffer_valid = 0; /* It's not valid @ the moment */
			aggregation->continuation = 1;
			aggregation->counter = 1;
			aggregation->section_length = EIT_SECTION_LENGTH(start);
			aggregation->offset = TS_PACKET_SIZE - start_of_pes; /* Offset for next TS packet */
			/* Copy first frame directly into buffer */
			memcpy(aggregation->buffer, pes_ptr + start_of_pes, TS_PACKET_SIZE - start_of_pes );
			/* Packet is not finished yet, it cannot be handled now */
	#ifdef DEBUG
			fprintf (stderr, "%s: Found multi-frame-table 0x%2.2x (last table 0x%2.2x), Section-Length: %d, Section: %d\n",
				channel_name(type), EIT_PACKET_TABLEID(start), EIT_PACKET_LAST_TABLEID(start), EIT_SECTION_LENGTH(start), EIT_SECTION_NUMBER(start)); 
	#endif 
			return 0; 
		}
		return 1; 
	} else {
		return 1; 
	}
	return 0;
}

static void extract_sdt_payload(unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes ) {
    unsigned char* start = NULL;
    start = pes_ptr + start_of_pes;
	/* collect up continuation frames */
	if (! collect_continuation(sdt_table, pes_ptr, pes_len, start_of_pes, CHANNEL_TYPE_SDT)) {
		return;
	}
	start = sdt_table->buffer; 
	/* SDT can use most of the PMT stuff */
#ifdef DEBUG
    fprintf (stderr, "SDT: Found data, table 0x%2.2x (Section length %d), program number %d, section %d, last section %d\n",
		PMT_TABLE_ID(start),
		PMT_SECTION_LENGTH(start),
		PMT_PROGRAM_NUMBER(start),	
		PMT_SECTION_NUMBER(start),
		PMT_LAST_SECTION_NUMBER(start));
#endif 
	unsigned char * description_offset = SDT_FIRST_DESCRIPTOR(start);
#ifdef DEBUG
	if (1) {
#else 
	if (strlen(global_state->station_name) == 0) {
#endif 
		if (crc32(start, sdt_table->section_length + 3) != 0) {
			fprintf(stderr, "SDT: crc32 does not match, calculated %d, expected 0\n", crc32(start, sdt_table->section_length + 3)); 

		} else {
			if ( PMT_TABLE_ID(start) == 0x42 ) {
				/* Table 0x42 contains information about current stream, we only want programm "running" (see mpeg standard for this hardcoded stuff) */
				if (SDT_DESCRIPTOR_RUNNING(description_offset) == 0x4) {
					unsigned char* description_content = SDT_DESCRIPTOR_CONTENT(description_offset);
					char provider_name[STR_BUF_SIZE]; 
					char service_name[STR_BUF_SIZE];
					uint8_t service_name_length = description_content[SDT_DC_PROVIDER_NAME_LENGTH(description_content) + 4]; 
					unsigned char * tmp = description_content + SDT_DC_PROVIDER_NAME_LENGTH(description_content) + 5; 
					// fprintf(stderr, "SDT: Dumping full buffer .. \n");
					// write(2, start, sdt_table->section_length); 
					// fprintf(stderr, "\n");
					/* Service 0x02, 0x0A, 0x07: (Digital) Radio */
					if (SDT_DC_SERVICE_TYPE(description_content) == 0x2
						|| SDT_DC_SERVICE_TYPE(description_content) == 0x0a 
						|| SDT_DC_SERVICE_TYPE(description_content) == 0x07 ) {
						if (SDT_DC_PROVIDER_NAME_LENGTH(description_content) < STR_BUF_SIZE) {
							if (SDT_DC_PROVIDER_NAME(description_content)[0] < 0x20) {
								/* MPEG Standard has very sophisticated charset encoding, therefore a simple Hack for my setup */
								snprintf(provider_name, SDT_DC_PROVIDER_NAME_LENGTH(description_content), "%s", SDT_DC_PROVIDER_NAME(description_content + 1) ); 
							} else {
								snprintf(provider_name, SDT_DC_PROVIDER_NAME_LENGTH(description_content), "%s", SDT_DC_PROVIDER_NAME(description_content));
							}
						}
						/* like above, but written compacted, if first character is smaller 0x20 it's the charset encoding */
						snprintf(service_name, service_name_length + (tmp[0]< 0x20?0:1), "%s", tmp + (tmp[0]< 0x20?1:0));
						/* Sometime we get garbage only store if we have a service_name with length > 0 */
						if (strlen(service_name) > 0) {
							/* Yes, we want to get information about the programme */
							output_logmessage("SDT: Stream is station %s from network %s.\n", service_name, provider_name);
							strncpy(global_state->station_name, service_name, STR_BUF_SIZE); 
						}
					} else {
						output_logmessage("SDT: Warning: Stream (also) contains unkown service with id 0x%2x\n", SDT_DC_SERVICE_TYPE(description_content)); 
					}
				}
			}
		}
	}
	sdt_table->buffer_valid = 0; 
	return;
}



static void extract_eit_payload(unsigned char *pes_ptr, size_t pes_len, ts2shout_channel_t *chan, int start_of_pes )
{
	unsigned char* start = NULL;
	char short_description[STR_BUF_SIZE]; 
	char text_description[STR_BUF_SIZE]; 

	memset(short_description, 0, STR_BUF_SIZE); 
	memset(text_description, 0, STR_BUF_SIZE); 
	
	start = pes_ptr + start_of_pes;
	/* collect up continuation frames */
	if (! collect_continuation(eit_table, pes_ptr, pes_len, start_of_pes, CHANNEL_TYPE_EIT)) {
		return;
	}
	/* ok, frame should be valid, repoint start */
	start = eit_table->buffer;
#ifdef DEBUG
	if (eit_table->buffer_valid == 1) {
		// fprintf(stderr, "EIT: Dumping full buffer .. \n");
		// write(2, eit_table->buffer, eit_table->section_length); 
		// fprintf(stderr, "\n");
	}
#endif 
	/* 0x4e current_event table */
	if (eit_table->buffer_valid == 1 &&  0x4e == EIT_PACKET_TABLEID(start)) {
		/* Current programme found */
		unsigned char* event_start = EIT_PACKET_EVENTSP(start);
		unsigned char* description_start = EIT_EVENT_DESCRIPTORP(event_start);
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
		/* 0x4d = Short event descriptor found */
		if (EIT_DESCRIPTOR_TAG(description_start) == 0x4d) {
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
			int text1_len = 0; 
			stringlen = EIT_NAME_LENGTH(description_start); 
			snprintf(short_description, stringlen, "%s", EIT_NAME_CONTENT(description_start) + 1);
			while (current_in_position + 60 <= max_size && current_in_position < eit_table->section_length) {
				stringlen = EIT_NAME_LENGTH(description_start); 
				/* Avoid the character code marker 0x05 for latin1 */
				text1_start = description_start + EIT_SIZE_DESCRIPTOR_HEADER + stringlen;
				text1_len = text1_start[0];
				if (text1_len == 0 || text1_len > max_size) break;
				memcpy(text_description + current_out_position, text1_start + 2, text1_len - 1);
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
				fprintf(stderr, "DEBUG: text1: %s, in_pos: %d (%d), text1_len: %d, global_len: %d\n", text_description, current_in_position,
					current_in_position + text1_len + EIT_SIZE_DESCRIPTOR_HEADER + stringlen, text1_len, max_size);
#endif
				description_start = description_start + EIT_SIZE_DESCRIPTOR_HEADER + stringlen + 1 + text1_len + 1; 
				current_in_position += EIT_SIZE_DESCRIPTOR_HEADER + stringlen + 1 + text1_len + 1;
			}
			/* Replace control characters */
			cleanup_mpeg_string(text_description);
			if ( EIT_EVENT_RUNNING_STATUS(event_start) == 4) {
				char tmp_title[STR_BUF_SIZE]; 
#ifdef DEBUG				
				fprintf(stderr, "EIT: RUNNING EVENT: %s (%s) in language %3.3s found.\n", short_description, text_description, EIT_DESCRIPTOR_LANG(first_description_start)); 
#endif			/* Write full info into channel title, but only if there is a difference between text and short description */
				if (text_description != NULL && strlen(text_description) > 0 ) {
					snprintf(tmp_title, STR_BUF_SIZE - 1, "%s - %s", short_description, text_description); 
				} else {
					/* Sonst nur short description */
					snprintf(tmp_title, STR_BUF_SIZE - 1, "%s", short_description);
				}
				if (0 != strcmp(tmp_title, global_state->stream_title)) {
					// It's needed in iso8859-1 for StreamTitle, but in UTF-8 for logging
					unsigned char utf8_message[STR_BUF_SIZE];
					strcpy(global_state->stream_title, tmp_title); 
					output_logmessage("EIT: Current transmission `%s'\n", utf8((unsigned char*)short_description, utf8_message)); 
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
	
	
	// Start of a PES header?
	if ( start_of_pes ) {
		

		// Parse the PES header
		es_ptr = parse_pes( pes_ptr, pes_len, &es_len, chan );

	
		/***** 
		   Having problems staying in sync with the PES stream 
		   So don't even try for the time being
		******/

		// Does PES stream have timestamp attached?
		/*if (chan->pes_ts) {
			unsigned long buf_dur = 0;
			
			// Calulate the duration of the frames already in the buffer
			if (chan->buf_used) {
				int frames_in_buffer = chan->buf_used/chan->mpah.framesize;
				buf_dur = ((chan->mpah.samples * 90000) / chan->mpah.samplerate) * frames_in_buffer;
			}
	

			// Make sure the RTP timestamp is in sync
			int ts_diff = (chan->rtp_ts+buf_dur) - chan->pes_ts;
			if (ts_diff) {
				if (chan->synced && chan->rtp_ts != 0) {
					fprintf(stderr, "Warning: PES TS != RTP TS (pid: %d, diff=%d)\n", chan->pid, ts_diff);
				}
				chan->rtp_ts = chan->pes_ts-buf_dur;
			}
			
		}*/

		
	} else if (chan->pes_stream_id) {
	
		// Don't output any data until we have seen a PES header
		es_ptr = pes_ptr;
		es_len = pes_len;
	
		// Are we are the end of the PES packet?
		if (es_len>chan->pes_remaining) {
			es_len=chan->pes_remaining;
		}
		
	}
	
	// Subtract the amount remaining in current PES packet
	chan->pes_remaining -= es_len;

	
	// Got some data to write out?
	if (es_ptr) {
	
		// Scan through Elementary Stream (ES) 
		// and try and find MPEG audio stream header
		while (!chan->synced && es_len>=4) {
		
			// Valid header?
			if (mpa_header_parse(es_ptr, &chan->mpah)) {

				// Now we know bitrate etc, set things up
				output_logmessage("Synced to MPEG audio in PID %d stream: 0x%x\n", chan->pid, chan->pes_stream_id );
				mpa_header_print( &chan->mpah );
				chan->synced = 1;
				
				// Work out how big payload will be
				if (chan->rtp_mtu < chan->mpah.framesize) {
					output_logmessage("Error: audio frame size %d is bigger than packet MTU %d.\n", chan->mpah.framesize, chan->rtp_mtu);
					exit(-1);
				}
				
				// Calculate the number of frames per packet
				chan->frames_per_packet = ( chan->rtp_mtu / chan->mpah.framesize );
				chan->payload_size = chan->frames_per_packet * chan->mpah.framesize;
				/* 
				fprintf(stderr, "  RTP payload size: %d (%d frames of audio)\n", 
					chan->payload_size, chan->frames_per_packet );
				*/
			    
				
				// Allocate buffer to store packet in
				chan->buf_size = chan->payload_size + TS_PACKET_SIZE;
				chan->buf = realloc( chan->buf, chan->buf_size + 4 );
				if (chan->buf==NULL) {
					output_logmessage("Error: Failed to allocate memory for MPEG Audio buffer\n");
					exit(-1);
				}
				chan->buf_ptr = chan->buf;
				
				// Initialise the RTP TS to the PES TS
				chan->rtp_ts = chan->pes_ts;
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
	
	
	// Got enough to send packet and we are allowed to output data
	if (chan->buf_used > chan->payload_size && global_state->output_payload ) {
		// Ensure a MPEG Audio frame starts here
		// this makes problems with programmes like "Radio Bob!" (44100 kHz, 256 kBit/s bitrate)
		// if (chan->buf_ptr[0] != 0xFF) {
		// 	output_logmessage("extract_pes_payload: Warning, lost MPEG Audio sync for PID %d.\n", chan->pid);
		//	chan->synced = 0;
		//	chan->buf_used = 0;
		//	return bytes_written;
		// }
		
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
					snprintf(streamtitle, STR_BUF_SIZE -1, "StreamTitle='%s';", global_state->stream_title);
					strcpy(global_state->old_stream_title, global_state->stream_title); 
				} else {
					memset(streamtitle, 0, strlen(global_state->stream_title)); 
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
		// Timestamp for MPEG Audio is based on fixed 90kHz clock rate
		chan->rtp_ts += ((chan->mpah.samples * 90000) / chan->mpah.samplerate)
							* chan->frames_per_packet;

		// Move any remaining memory to the start of the buffer
		chan->buf_used -= chan->payload_size;
		memmove( chan->buf_ptr, chan->buf_ptr+chan->payload_size, chan->buf_used );
		
	}
	return bytes_written; 
}

/* This is a helper function for the processing loop of process_ts_packet
 * it resets the the data collector if the pid changes */
void pid_change() {
	//memset(eit_table, 0, sizeof(section_aggregate_t)); 
	// memset(sdt_table, 0, sizeof(section_aggregate_t)); 
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
			// trying to start over, to get a full read by waiting a little bit (150 ms)
			poll(NULL, 0, 150);
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
	if (!global_state->output_payload) {
		/* not all data items were available, check wether they are available now. 
		 * output the header and START playing audio */
		if (global_state->station_name 
			&& strlen(global_state->station_name) > 0 
			&& global_state->br > 0 
			&& global_state->sr > 0) {
			if (shoutcast) {
				snprintf(header, STR_BUF_SIZE, "Content-Type: audio/mpeg\n" \
						"Connection: close\n" \
						"icy-br: %d\n" \
						"icy-sr: %d\n" \
						"icy-name: %s\n" \
						"icy-metaint: %d\n\n",
						global_state->br * 1000, global_state->sr, global_state->station_name, SHOUTCAST_METAINT); 
			} else {
				snprintf(header, STR_BUF_SIZE, "Content-Type: audio/mpeg\n" \
						"Connection: close\n\n"); 
			}
			fwrite(header, strlen(header), 1, stdout); 
			fflush(stdout); 
			global_state->output_payload = 1; 
		}
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
	if (getenv("HTTP_USER_AGENT")) {
		snprintf(user_agent_string, STR_BUF_SIZE, "User-Agent: ts2shout for %s", getenv("HTTP_USER_AGENT")); 
	} else {
		snprintf(user_agent_string, STR_BUF_SIZE, "User-Agent: ts2shout");
	}
	if (getenv("REMOTE_ADDR")) {
		char s[STR_BUF_SIZE];
		sprintf(s, "Forwarded: by \"%s\"; for \"%s\"; proto=http", "127.0.0.1", getenv("REMOTE_ADDR")); 
		header = curl_slist_append(header, s);
	}
    header = curl_slist_append(header, user_agent_string);
    int res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header);
	/* generate URI (TVHEADEND and PROGRAMMNO was checked by calling function) */
	if (getenv("REDIRECT_TVHEADEND")) {
		if (strncmp(getenv("REDIRECT_TVHEADEND"), "http://", 7) == 0) {
			snprintf(url, STR_BUF_SIZE, "%s/%s", getenv("REDIRECT_TVHEADEND"), getenv("REDIRECT_PROGRAMMNO")); 
		} else {
			snprintf(url, STR_BUF_SIZE, "http://%s/%s", getenv("REDIRECT_TVHEADEND"), getenv("REDIRECT_PROGRAMMNO"));
		}
	} else {
		if (strncmp(getenv("TVHEADEND"), "http://", 7) == 0) {
			snprintf(url, STR_BUF_SIZE, "%s/%s", getenv("TVHEADEND"), getenv("PROGRAMMNO")); 
		} else {
			snprintf(url, STR_BUF_SIZE, "http://%s/%s", getenv("TVHEADEND"), getenv("PROGRAMMNO"));
		}
	}
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
		pes_ptr += (TS_PACKET_ADAPT_LEN(buf) + 1);
		pes_len -= (TS_PACKET_ADAPT_LEN(buf) + 1);
	}
	/* this frame: different PID then last frame? Then "pid_change" */
	if (pid != global_state->last_pid) {
		pid_change();
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
				extract_eit_payload ( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
				// global_state->ts_sync_error = 0; /* no pid_change() because it resets the EIT stuff */
				break;
			case CHANNEL_TYPE_SDT: 
				extract_sdt_payload ( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
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
	global_state->last_pid = pid; 
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
	} else {
		// Parse command line arguments
		parse_args( argc, argv );
	} 
	init_structures();
	output_logmessage("Streaming %s in %s mode.\n", (shoutcast?"with shoutcast StreamTitles":"without shoutcast support, mpeg only"), 
		(cgi_mode?"CGI":"FILTER"));
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

