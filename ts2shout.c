/* 

	dvbshout.c
	(C) Dave Chapman <dave@dchapman.com> 2001, 2002.
	(C) Nicholas J Humfrey <njh@aelius.com> 2006.
	reworked to be ts2shout.c
	(C) Carsten Gross <carsten@siski.de> 2018
	
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
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>

#include "ts2shout.h"
#include "config.h"

#define STDOUT 1 

int Interrupted=0;    /* Playing interrupted by signal? */
int channel_count=0;  /* Current listen channel count */
int shoutcast=1;      /* Send shoutcast headers? */
int logformat=1;	  /* Apache compatible output format */


dvbshout_channel_t *channel_map[MAX_PID_COUNT];
dvbshout_channel_t *channels[MAX_CHANNEL_COUNT];

static void signal_handler(int signum)
{
	if (signum != SIGPIPE) {
		Interrupted=signum;
	}
	signal(signum,signal_handler);
}

static void parse_args(int argc, char **argv) 
{
	if (argc == 2) {
		if (strcmp("noshout", argv[1]) == 0) {
			shoutcast = 0;
		}
	}
	#if 0
		fprintf(stderr,"%s version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
		fprintf(stderr,"Usage: dvbshout <configfile>\n\n");
		exit(-1);
	#endif 
	init_structures();
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

static void ts_continuity_check( dvbshout_channel_t *chan, int ts_cc ) 
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


static void extract_pat_payload(unsigned char *pes_ptr, size_t pes_len, dvbshout_channel_t *chan, int start_of_pes ) {
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
		} else {
#ifdef DEBUG
			fprintf (stderr, "PAT: crc32 does not match: calculated %d, expected 0\n", crc32(start, PAT_SECTION_LENGTH(start) + 3)); 
#endif	
		}
			
	}
#ifdef DEBUG
	fprintf (stderr, "PAT: Programme 1, MAP-PID %d\n", PAT_PROGRAMME_PMT(programmes)); 
#endif
	
}

static void extract_pmt_payload(unsigned char *pes_ptr, size_t pes_len, dvbshout_channel_t *chan, int start_of_pes ) {
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
			/* Everything matched, now we calculate the crc32 of the PMT frame, to avoid decoding garbage */
			if (crc32(start, PAT_SECTION_LENGTH(start) + 3) == 0) {	
				if (PMT_STREAM_TYPE(pmt_stream_info_offset) == 0x03 /* MPEG 1 audio */
					|| PMT_STREAM_TYPE(pmt_stream_info_offset) == 0x04 /* MPEG 2 audio */
					|| PMT_STREAM_TYPE(pmt_stream_info_offset) == 0x0f /* MPEG 2 audio */) {
					/* Potential audio stream found! */
					if (! channel_map[PMT_PID(pmt_stream_info_offset)]) {
						add_channel(CHANNEL_TYPE_PAYLOAD, PMT_PID(pmt_stream_info_offset));
					}
				} else {
#ifdef DEBUG
					fprintf(stderr, "PMT: Stream with pid %d found, but unsusable format (stream type %d)\n", 
			                PMT_PID(pmt_stream_info_offset), PMT_STREAM_TYPE(pmt_stream_info_offset));
#endif
				}
			} else {
				/* CRC32 Error - ignore frame */
#ifdef DEBUG
				fprintf(stderr, "PMT: crc32 does not match: calculated %d, expected 0\n", crc32(start, PAT_SECTION_LENGTH(start) + 3)); 
#endif 
			}
		}
	}
}

static void extract_sdt_payload(unsigned char *pes_ptr, size_t pes_len, dvbshout_channel_t *chan, int start_of_pes ) {
    unsigned char* start = NULL;
    start = pes_ptr + start_of_pes;
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
	if (strlen(chan->name) == 0) {
		if (crc32(start, PMT_SECTION_LENGTH(start) + 3) != 0) {
#ifdef DEBUG
			fprintf(stderr, "SDT: crc32 does not match, calculated %d, expected 0\n", crc32(start, PMT_SECTION_LENGTH(start) + 3)); 
#endif 
			return;
		}
		if ( PMT_TABLE_ID(start) == 0x42 ) {
			/* Table 0x42 contains information about current stream, we only want programm "running" (see mpeg standard for this hardcoded stuff) */
			if (SDT_DESCRIPTOR_RUNNING(description_offset) == 0x4) {
				unsigned char* description_content = SDT_DESCRIPTOR_CONTENT(description_offset);
				char provider_name[STR_BUF_SIZE]; 
				char service_name[STR_BUF_SIZE];
				uint8_t service_name_length = description_content[SDT_DC_PROVIDER_NAME_LENGTH(description_content) + 4]; 
				unsigned char * tmp = description_content + SDT_DC_PROVIDER_NAME_LENGTH(description_content) + 6; 
				if (SDT_DC_PROVIDER_NAME_LENGTH(description_content) < STR_BUF_SIZE) {
					if (SDT_DC_PROVIDER_NAME(description_content)[0] < 0x20) {
						/* MPEG Standard has very sophisticated charset encoding, therefore a simple Hack for my setup */
						snprintf(provider_name, SDT_DC_PROVIDER_NAME_LENGTH(description_content), "%s", SDT_DC_PROVIDER_NAME(description_content + 1) ); 
					} else {
						snprintf(provider_name, SDT_DC_PROVIDER_NAME_LENGTH(description_content), "%s", SDT_DC_PROVIDER_NAME(description_content));
					}
				}
				snprintf(service_name, service_name_length, "%s", tmp);
				/* Sometime we get garbage only store if we have a service_name with length > 0 */
				if (strlen(service_name) > 0) {
					/* Yes, we want to get information about the programme */
					output_logmessage("SDT: Stream is station %s from network %s.\n", service_name, provider_name);
					strncpy(chan->name, service_name, STR_BUF_SIZE);
				}
			}
		}
	}
	return;
}

static void extract_eit_payload(unsigned char *pes_ptr, size_t pes_len, dvbshout_channel_t *chan, int start_of_pes )
{
	unsigned char* start = NULL;
	char short_description[255]; 
	char text_description[255]; 

	memset(short_description, 0, 255); 
	memset(text_description, 0, 255); 
	
	start = pes_ptr + start_of_pes;
#ifdef DEBUG
	fprintf (stderr, "EIT: Found table 0x%2.2x (last table 0x%2.2x), Section: %d, %p, %d\n", 
			EIT_PACKET_TABLEID(start), 
			EIT_PACKET_LAST_TABLEID(start),
			EIT_SECTION_NUMBER(start), 
			pes_ptr, start_of_pes); 
#endif
	/* 0x4e current_event table */
	if ( 0x4e == EIT_PACKET_TABLEID(start)) {
		/* Current programme found */
		unsigned char* event_start = EIT_PACKET_EVENTSP(start);
		unsigned char* description_start = EIT_EVENT_DESCRIPTORP(event_start);
#ifdef DEBUG
		fprintf(stderr, "EIT: Found event with id %d, currently in status %d, starttime %6.6x, duration %6.6x.\n",
			EIT_EVENT_EVENTID(event_start),
			EIT_EVENT_RUNNING_STATUS(event_start),
			EIT_EVENT_STARTTIME_TIME(event_start),
			EIT_EVENT_DURATION(event_start));
#endif 
		/* Check crc32 - seems not to work for all programmes? */
		/* 
		if (crc32((char *)start, EIT_SECTION_LENGTH(start)+3) != 0) {
#ifdef DEBUG
			fprintf(stderr, "EIT: crc32 does not match, calculated %d, expected 0\n", crc32((char *)start, EIT_SECTION_LENGTH(start)+3)); 
#endif 
			return; 
		}
		*/ 
		/* 0x4d = Short event descriptor found */
		if (EIT_DESCRIPTOR_TAG(description_start) == 0x4d) {
			unsigned int stringlen = EIT_NAME_LENGTH(description_start); 
			snprintf(short_description, stringlen, "%s", EIT_NAME_CONTENT(description_start) + 1);
			/* Avoid the character code marker 0x05 for latin1 */
			unsigned char* text_start = description_start + EIT_SIZE_DESCRIPTOR_HEADER + stringlen;
			int text_len = text_start[0];
#ifdef DEBUG
			fprintf(stderr, "DEBUG: text_len: %d\n", text_len);
#endif
			snprintf(text_description, text_len, "%s", text_start + 2); 
			if ( EIT_EVENT_RUNNING_STATUS(event_start) == 4) {
#ifdef DEBUG				
				fprintf(stderr, "EIT: RUNNING EVENT: %s (%s) in language %3.3s found.\n", short_description, text_description, EIT_DESCRIPTOR_LANG(description_start)); 
#endif			/* Write full info into channel title, but only if there is a difference between text and short description */
				if (text_description != NULL && strlen(text_description) > 0 ) {
					snprintf(chan->title, STR_BUF_SIZE - 1, "%s - %s", short_description, text_description); 
				} else {
					/* Sonst nur short description */
					snprintf(chan->title, STR_BUF_SIZE - 1, "%s", short_description);
				}
			} else {
#ifdef DEBUG
				fprintf(stderr, "EIT: Event %s in Language %3.3s found.\n", short_description, EIT_DESCRIPTOR_LANG(description_start));
#endif 
			}
		}
	}
	return;
}

static unsigned long int extract_pes_payload( unsigned char *pes_ptr, size_t pes_len, dvbshout_channel_t *chan, int start_of_pes ) 
{
	unsigned char* es_ptr=NULL;
	size_t es_len=0;
	
	unsigned long int bytes_written = 0; 	
	
	
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
		if (chan->synced) {
		
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
	
	
	// Got enough to send packet?
	if (chan->buf_used > chan->payload_size ) {
		
		// Ensure a MPEG Audio frame starts here
		if (chan->buf_ptr[0] != 0xFF) {
			output_logmessage("extract_pes_payload: Warning, lost MPEG Audio sync for PID %d.\n", chan->pid);
			chan->synced = 0;
			chan->buf_used = 0;
			return bytes_written;
		}
		
		#ifndef DEBUG 
		/* If Icy-MetaData is set to 1 a shoutcast StreamTitle is required all 8192 */ 
		/* (SHOUTCAST_METAINT) Bytes */
		/* see documentation: https://cast.readme.io/docs/icy */
		if (shoutcast) {
			if (chan->payload_size + chan->bytes_written_nt <= SHOUTCAST_METAINT) {
				int retval = write(STDOUT, (char*)chan->buf, chan->payload_size); 
				/* No metaint required in this output */
				if ( retval < 0) {
					output_logmessage("write_streamdata: Error during write: %s, Exiting.\n", strerror(errno)); 
					/* Not ready */
					Interrupted = 1; 
					exit(1);
				} else if (0 == retval) {
					output_logmessage("write_streamdata: EOF on STDOUT(?) during write.\n"); 
				}
				chan->bytes_written_nt += chan->payload_size;
				bytes_written += chan->payload_size;
			} else {
				uint32_t first_write = SHOUTCAST_METAINT - chan->bytes_written_nt;
				uint32_t second_write = chan->payload_size - first_write; 
				uint16_t bytes = 0;
				char streamtitle[STR_BUF_SIZE]; 
				snprintf(streamtitle, STR_BUF_SIZE -1, "StreamTitle='%s';", channel_map[18]->title);
				bytes = ((strlen(streamtitle))>>4) + 1; /* Shift right by 4 bit => Divide by 16, and add 1 to get minimum possible length */
				bytes_written += write(STDOUT, (char*)chan->buf, first_write); 
				bytes_written += write(STDOUT, &bytes, 1);
				bytes_written += write(STDOUT, streamtitle, bytes<<4);
				bytes_written += write(STDOUT, (char*)(chan->buf + first_write), second_write);
				chan->bytes_written_nt = second_write;
			}
		} else {
			if ( write(STDOUT, (char*)chan->buf, chan->payload_size) <= 0) {
				 exit(1);
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

/* This is the main processing loop for the incoming stream data */

void process_ts_packets( int fd_dvr )
{
	unsigned char buf[TS_PACKET_SIZE];
	unsigned char* pes_ptr=NULL;
	unsigned int pid=0;
	size_t pes_len;
	int bytes_read;
	long int bytes_streamed_read = 0; 
	long int bytes_streamed_write = 0; 
	const long int mb_conversion = 1048576;
	uint16_t ts_sync_error = 0; 
	const uint16_t max_sync_errors = 5; 
	
	while ( !Interrupted ) {
		
		bytes_read = read(fd_dvr,buf,TS_PACKET_SIZE);
		bytes_streamed_read += bytes_read; 
		// fprintf(stderr, "Bytes read: %d, errorno=%d\n", bytes_read, errno);
		// if (bytes_read==0) continue;
		if (bytes_read == 0) {
			output_logmessage("process_ts_packets: read from stream %.2f MB, wrote %.2f MB, no bytes left to read - EOF. Exiting.\n", 
			                  (float)bytes_streamed_read/1000000, (float)bytes_streamed_write/mb_conversion);
			break; 
		}
		if (bytes_read > 0 && bytes_read < TS_PACKET_SIZE) {
			output_logmessage("process_ts_packets: short read, only got %d bytes, instead of %d, trying to resync\n", bytes_read, TS_PACKET_SIZE);
			// trying to start over, to get a full read by waiting a little bit (150 ms)
			poll(NULL, 0, 150);
			/* Throw rest of the not read bytes away by trying to read it in the buffer and jump back to read full mpeg frames */
			bytes_read = read(fd_dvr,buf,TS_PACKET_SIZE - bytes_read);
			ts_sync_error += 1;
			if (ts_sync_error > max_sync_errors) {
				break; 
			}
			continue;
		} else if (bytes_read < 0) {
			output_logmessage("process_ts_packets: streamed %ld bytes, read returned an error: %s, exiting.\n", bytes_streamed_read, strerror(errno)); 
			break;
		}
		// Check the sync-byte
		if (TS_PACKET_SYNC_BYTE(buf) != 0x47) {
			int i = 1;
			int did_read = 0; 
			for (i = 1; i < TS_PACKET_SIZE; i++) {
				if (0x47 == buf[i]) {
					bytes_read = read(fd_dvr, buf, i);
					did_read = i;
					continue; 
				}
			}
			if (0 == did_read) {
				output_logmessage("process_ts_packets: After reading %.2f MB and writing %.2f, Lost synchronisation - skipping full block (Lost counter %d, aborting at %d) \n", (float)bytes_streamed_read/mb_conversion, (float)bytes_streamed_write/mb_conversion, ts_sync_error, max_sync_errors);
			} else {
				output_logmessage("process_ts_packets: After reading %.2f MB and writing %.2f, Lost synchronisation - skipping %d bytes (Lost counter %d, aborting at %d) \n", (float)bytes_streamed_read/mb_conversion, (float)bytes_streamed_write/mb_conversion, did_read, ts_sync_error, max_sync_errors);
			}
			if (did_read > 0) {
				ts_sync_error += 1;
				if (ts_sync_error > max_sync_errors) {
					output_logmessage("process_ts_packets: After reading %.2f MB and writing %.2f, Lost synchronisation (sync loss counter of %d exceeded)\n", (float)bytes_streamed_read/mb_conversion, (float)bytes_streamed_write/mb_conversion, max_sync_errors);
					return; 
				}
				continue;
			}
			/* No 0x47 found, most likly we are completly lost */
			ts_sync_error += 1; 
			if (ts_sync_error > max_sync_errors) {
				output_logmessage("process_ts_packets: After reading %.2f MB and writing %.2f, Lost synchronisation (sync loss counter of %d exceeded)\n", (float)bytes_streamed_read/mb_conversion, (float)bytes_streamed_write/mb_conversion, max_sync_errors);
				return; 
			}
			continue;
		}
		
		// Get the PID of this TS packet
		pid = TS_PACKET_PID(buf);
		
		// Transport error?
		if ( TS_PACKET_TRANS_ERROR(buf) ) {
			output_logmessage("process_ts_packets: Warning, transport error in PID %d.\n", pid);
			if (channel_map[ pid ]) {
				channel_map[ pid ]->synced = 0;
				channel_map[ pid ]->buf_used = 0;
			}
			continue;
		}			

		// Scrambled?
		if ( TS_PACKET_SCRAMBLING(buf) ) {
			fprintf(stderr, "Warning: PID %d is scrambled.\n", pid);
			continue;
		}	

		// Location of and size of PES payload
		pes_ptr = &buf[4];
		pes_len = TS_PACKET_SIZE - 4;

		// Check for adaptation field?
		if (TS_PACKET_ADAPTATION(buf)==0x1) {
			// Payload only, no adaptation field
		} else if (TS_PACKET_ADAPTATION(buf)==0x2) {
			// Adaptation field only, no payload
			continue;
		} else if (TS_PACKET_ADAPTATION(buf)==0x3) {
			// Adaptation field AND payload
			pes_ptr += (TS_PACKET_ADAPT_LEN(buf) + 1);
			pes_len -= (TS_PACKET_ADAPT_LEN(buf) + 1);
		}
		
		// Check we know about the payload
		if (channel_map[ pid ]) {
			// Continuity check
			ts_continuity_check( channel_map[ pid ], TS_PACKET_CONT_COUNT(buf) );
			enum_channel_type channel_type = channel_map[ pid ]->channel_type; 
			switch (channel_type) {
				case CHANNEL_TYPE_PAT: 
					extract_pat_payload(pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
					ts_sync_error = 0; 
					break;
				case CHANNEL_TYPE_EIT: 
					extract_eit_payload ( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
					ts_sync_error = 0;
					break;
				case CHANNEL_TYPE_SDT: 
					extract_sdt_payload ( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
					ts_sync_error = 0; 
					break;
				case CHANNEL_TYPE_PMT: 
					extract_pmt_payload ( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
					ts_sync_error = 0; 
					break;
				case CHANNEL_TYPE_PAYLOAD: 
					bytes_streamed_write += extract_pes_payload( pes_ptr, pes_len, channel_map[ pid ], TS_PACKET_PAYLOAD_START(buf) );
					ts_sync_error = 0; 
					break; 
				default: 
#ifdef DEBUG
					fprintf(stderr, "Warning: don't know anything about PID %d.\n", pid);
#endif 
				break;
			}
		}
	}
}



int main(int argc, char **argv)
{
	int fd_frontend=-1;
	int fd_dvr=-1;
	int i;
	
	
	// Initialise data structures
	for (i=0;i<MAX_PID_COUNT;i++) channel_map[i]=NULL;
	for (i=0;i<MAX_CHANNEL_COUNT;i++) channels[i]=NULL;
	
	// Parse command line arguments
	parse_args( argc, argv );

	// Open the DRV device
	if((fd_dvr = open("/dev/stdin", O_RDONLY)) < 0){
		perror("Failed to open STDIN device");
		return -1;
	}

	output_logmessage("Streaming %s.\n", (shoutcast?"with shoutcast StreamTitles":"without shoutcast support, mpeg only"));

	// Setup signal handlers
	if (signal(SIGHUP, signal_handler) == SIG_IGN) signal(SIGHUP, SIG_IGN);
	if (signal(SIGINT, signal_handler) == SIG_IGN) signal(SIGINT, SIG_IGN);
	if (signal(SIGTERM, signal_handler) == SIG_IGN) signal(SIGTERM, SIG_IGN);

	process_ts_packets( fd_dvr );
	

	if (Interrupted) {
		output_logmessage("Caught signal %d - closing cleanly.\n", Interrupted);
	}
	
	
	// Clean up
	for (i=0;i<channel_count;i++) {
		if (channels[i]->fd != -1) close(channels[i]->fd);
		if (channels[i]->buf) free( channels[i]->buf );
		free( channels[i] );
	}
	close(fd_dvr);
	close(fd_frontend);

	return(0);
}

