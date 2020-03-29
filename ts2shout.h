/* ts2shout.h

   Copyright (C) Nicholas Humfrey 2006, Dave Chapman 2002
   reworked to be ts2shout.h
   (C) Carsten Gross <carsten@siski.de> 2018


   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
   Or, point your browser to http://www.gnu.org/copyleft/gpl.html

*/

#ifndef _TS2SHOUT_H
#define _TS2SHOUT_H

#include <stdint.h>

#include "mpa_header.h"


// The size of MPEG2 TS packets
#define TS_PACKET_SIZE			188
// The size of MPEG2 TS headers
#define TS_HEADER_SIZE			4

// There seems to be a limit of 32 simultaneous filters in the driver
#define MAX_CHANNEL_COUNT		32

// Maximum allowed PID value
#define MAX_PID_COUNT			8192

// Standard string buffer size
#define STR_BUF_SIZE			6000
#define EIT_BUF_SIZE			5000

/* Shoutcast Interval to next metadata */
#define SHOUTCAST_METAINT		8192

/*
	Macros for accessing MPEG-2 TS packet headers
*/
#define TS_PACKET_SYNC_BYTE(b)		(b[0])
#define TS_PACKET_TRANS_ERROR(b)	((b[1]&0x80)>>7)
#define TS_PACKET_PAYLOAD_START(b)	((b[1]&0x40)>>6)
#define TS_PACKET_PRIORITY(b)		((b[1]&0x20)>>4)
#define TS_PACKET_PID(b)			(((b[1]&0x1F)<<8) | b[2])

#define TS_PACKET_SCRAMBLING(b)		((b[3]&0xC0)>>6)
#define TS_PACKET_ADAPTATION(b)		((b[3]&0x30)>>4)
#define TS_PACKET_CONT_COUNT(b)		((b[3]&0x0F)>>0)
#define TS_PACKET_ADAPT_LEN(b)		(b[4])
#define TS_PACKET_POINTER(b)		(uint8_t)((((b[1]&0x40)>>6 == 1) && (b[4] < 183))? b[4] : 0)

/* 
	Macros for access MPEG-2 EIT packets
	http://www.etsi.org/deliver/etsi_en/300400_300499/300468/01.14.01_60/en_300468v011401p.pdf 
	Page 28
*/
#define EIT_PACKET_TABLEID(b)		(b[0])
#define EIT_SECTION_LENGTH(b) (uint16_t)(((b[1] & 0x0f)<<8)|b[2])
#define EIT_SERVICE_ID(b)			(uint32_t)((b[3]<<8) | b[4])
#define EIT_VERSION_NUMBER(b)		(uint8_t)((b[5] & 0x3e)>>1 )
#define EIT_SECTION_NUMBER(b)		(b[6])
#define EIT_LAST_SECTION_NUMBER(b)	(b[7])
#define EIT_TRANSPORT_STREAM_ID(b)	(uint32_t)(b[8]<<8 | b[9])
/* original network +2 Byte */
#define EIT_PACKET_LAST_TABLEID(b)		(b[13])
#define EIT_PACKET_EVENTSP(b)		(b + 14)

/* Macros for accessing the Eventinformation */
#define EIT_EVENT_EVENTID(b)		(uint16_t)((b[0]<<8) | ( b[1]))
#define EIT_EVENT_STARTTIME_TIME(b)	(uint32_t)(b[4]<<16 | b[5]<<8 | b[6])
#define EIT_EVENT_STARTTIME_DATE(b) (uint32_t)(b[2]<<8 | b[3])
#define EIT_EVENT_DURATION(b)		(uint32_t)(b[7]<<16 | b[8]<<8 | b[9])
#define EIT_EVENT_RUNNING_STATUS(b)	((b[10] & 0xe0)>>5)
#define EIT_EVENT_LOOPLENGTH(b)		(uint16_t)( ((b[10]&0x0f)<<8) | b[11] )
#define EIT_EVENT_DESCRIPTORP(b)	(b + 12)

#define EIT_DESCRIPTOR_TAG(b)		(b[0]) 
#define EIT_DESCRIPTOR_LENGTH(b)	(b[1])
/* #define EIT_DESCRIPTOR_STREAM_CONTENT(b)	(b[2] & 0xf)
#define EIT_DESCRIPTOR_COMPONENT_TYPE(b) (b[3])
#define EIT_DESCRIPTOR_COMPONENT_TAG(b) (b[4])
*/
/* 3 Byte Language code */
#define EIT_SIZE_DESCRIPTOR_HEADER	6
#define EIT_DESCRIPTOR_LANG(b)		(b + 2)
#define EIT_NAME_LENGTH(b)			(b[5])
#define EIT_NAME_CONTENT(b)			(b + 6)

/* Macros for accessing PAT frames */

#define PAT_TABLE_ID(b) (b[0])
#define PAT_SECTION_LENGTH(b) (uint16_t)((b[1] & 0xf)<<8 | b[2])
#define PAT_TRANSPORT_STREAM_ID(b) (uint16_t)(b[3]<<8 | b[4])
#define PAT_SECTION_NUMBER(b) (b[6])
#define PAT_LAST_SECTION_NUMBER(b) (b[7])
#define PAT_PROGRAMME_START(b) (b + 8) 
#define PAT_PROGRAMME_PMT(b) ((uint16_t)( (b[2]&0x1f) <<8 | b[3]))

/* Macros for accessing PMT frames */
#define PMT_TABLE_ID(b) (b[0])
#define PMT_SECTION_LENGTH(b) (uint16_t)((b[1] & 0xf)<<8 | b[2])
#define PMT_PROGRAM_NUMBER(b) (uint16_t)(b[3]<<8 | b[4])
#define PMT_SECTION_NUMBER(b) (b[6])
#define PMT_LAST_SECTION_NUMBER(b) (b[7])
#define PMT_PCR_PID(b) ((uint16_t)( (b[8]&0x1f) <<8 | b[9]))
#define PMT_PROGRAMME_INFO_LENGTH(b) ((uint16_t)( (b[10]&0x0f) <<8 | b[11]))
#define PMT_DESCRIPTOR(b) (b + 12)
/* Macros for accessing PMT stream info inside PMT */
#define PMT_STREAM_TYPE(b) (b[0])
#define PMT_PID(b) ((uint16_t)((b[1] & 0x1f)<<8 | b[2]))
#define PMT_INFO_LENGTH(b) ((uint16_t)((b[3] & 0x0f)<<8 | b[4]))

/* Macros for accessing PMT descriptors */
#define PMT_FIRST_STREAM_DESCRIPTORP(b) (b + 5)
#define PMT_NEXT_STREAM_DESCRIPTOROFF(b) (b[1] + 2)
#define PMT_STREAM_DESCRIPTOR_TAG(b) (b[0])


/* Macros for accessing SDT frames */
/* Everything before can be reused from PMT macros */
#define SDT_ORIGINAL_NETWORK_ID(b) (uint16_t)(b[8]<<8 | b[9])
/* b[11] future use */
#define SDT_FIRST_DESCRIPTOR(b) (b + 11)
#define SDT_DESCRIPTOR_SERVICE_ID(b)	(uint16_t)(b[0]<<8 | b[1])
#define SDT_DESCRIPTOR_RUNNING(b)       ((b[3] & 0xc0)>>5)
#define SDT_DESCRIPTOR_LOOP_LENGTH(b)   (uint16_t)((b[3] & 0x0f)<<8 | b[4])
#define SDT_DESCRIPTOR_CONTENT(b)		(b + 5)
/* Descriptor of the SDT */
/* DC = DESCRIPTOR_CONTENT */
#define SDT_DC_TAG(b) (b[0])
#define SDT_DC_LENGTH(b) (b[1])
#define SDT_DC_SERVICE_TYPE(b) (b[2])
#define SDT_DC_PROVIDER_NAME_LENGTH(b) (b[3])
#define SDT_DC_PROVIDER_NAME(b) (b+4)

/* Macros for accessing MPEG-2 PES packet headers */
#define PES_PACKET_SYNC_BYTE1(b)	(b[0])
#define PES_PACKET_SYNC_BYTE2(b)	(b[1])
#define PES_PACKET_SYNC_BYTE3(b)	(b[2])
#define PES_PACKET_STREAM_ID(b)		(b[3])
#define PES_PACKET_LEN(b)			((b[4] << 8) | b[5])

#define PES_PACKET_SYNC_CODE(b)		((b[6] & 0xC0) >> 6)
#define PES_PACKET_SCRAMBLED(b)		((b[6] & 0x30) >> 4)
#define PES_PACKET_PRIORITY(b)		((b[6] & 0x08) >> 3)
#define PES_PACKET_ALIGNMENT(b)		((b[6] & 0x04) >> 2)
#define PES_PACKET_COPYRIGHT(b)		((b[6] & 0x02) >> 1)
#define PES_PACKET_ORIGINAL(b)		((b[6] & 0x01) >> 0)

#define PES_PACKET_PTS_DTS(b)		((b[7] & 0xC0) >> 6)
#define PES_PACKET_ESCR(b)			((b[7] & 0x20) >> 5)
#define PES_PACKET_ESR(b)			((b[7] & 0x10) >> 4)
#define PES_PACKET_DSM_TRICK(b)		((b[7] & 0x8) >> 3)
#define PES_PACKET_ADD_COPY(b)		((b[7] & 0x4) >> 2)
#define PES_PACKET_CRC(b)			((b[7] & 0x2) >> 1)
#define PES_PACKET_EXTEN(b)			((b[7] & 0x1) >> 0)
#define PES_PACKET_HEAD_LEN(b)		(b[8])

#define PES_PACKET_PTS(b)		((uint32_t)((b[9] & 0x0E) << 29) | \
					 (uint32_t)(b[10] << 22) | \
					 (uint32_t)((b[11] & 0xFE) << 14) | \
					 (uint32_t)(b[12] << 7) | \
					 (uint32_t)(b[13] >> 1))

#define PES_PACKET_DTS(b)		((uint32_t)((b[14] & 0x0E) << 29) | \
					 (uint32_t)(b[15] << 22) | \
					 (uint32_t)((b[16] & 0xFE) << 14) | \
					 (uint32_t)(b[17] << 7) | \
					 (uint32_t)(b[18] >> 1))

#define FOREACH_CHANNEL_TYPE(CHANNEL_TYPE) \
        CHANNEL_TYPE(CHANNEL_TYPE_PAT)  \
        CHANNEL_TYPE(CHANNEL_TYPE_SDT)  \
        CHANNEL_TYPE(CHANNEL_TYPE_EIT)  \
        CHANNEL_TYPE(CHANNEL_TYPE_PMT)  \
		CHANNEL_TYPE(CHANNEL_TYPE_PAYLOAD) \

#define GENERATE_ENUM(ENUM) ENUM,
#define GENERATE_STRING(STRING) #STRING,


typedef enum {
	FOREACH_CHANNEL_TYPE(GENERATE_ENUM)
} enum_channel_type;

/* Structure containing single channel */
typedef struct ts2shout_channel_s {
	int num;				// channel number
	int pid;				// Packet Identifier of stream

	enum_channel_type channel_type;	// Channel Type (MPEG / PMT / whatever) 
	
	int continuity_count;	// TS packet continuity counter

	/* Only relevant for the payload stream */	
	int pes_stream_id;		// PES stream ID
	size_t pes_remaining;	// Number of bytes remaining in current PES packet
	unsigned long pes_ts;	// Timestamp for current PES packet
	mpa_header_t mpah;		// Parsed MPEG audio header
	int synced;				// Have MPA sync?
	uint32_t  bytes_written_nt; // Bytes written (count the 8192 Bytes to next StreamTitle inside shoutcast)
	uint8_t * buf;			// MPEG Audio Buffer (with 4 nulls bytes)
	uint8_t * buf_ptr;		// Pointer to start of audio data
	uint32_t buf_size;		// Usable size of MPEG Audio Buffer
	uint32_t buf_used;		// Amount of buffer used

	int payload_size;				// Size of the payload
} ts2shout_channel_t;

/* global information about the received programme */
typedef struct programm_info_s {
	uint8_t	info_available;				/* Information available */
 	uint8_t payload_added;				/* Payload is added, no longer scan PAT and PMT */
	uint8_t	output_payload;				/* All header information is known, therefore audiodata can be read and output (for CGI mode) */
	uint8_t sdt_fromstream;				/* Station name fetched from stream */
	uint8_t	cache_written;				/* cache written in current session */
	char station_name[STR_BUF_SIZE];	/* Name of station (normally only a few bytes) */
	char stream_title[STR_BUF_SIZE];	/* StreamTitle for shoutcast stream */
	char old_stream_title[STR_BUF_SIZE]; /* Old StreamTitle, that was given in last session */
	uint32_t br;						/* Bitrate of stream e.g. 320000 kBit/s			*/
	uint32_t sr;						/* Streamrate of stream e.g. 48 kHz == 48000 Hz */
    uint64_t bytes_streamed_read;		/* Total bytes read from stream */
    uint64_t bytes_streamed_write;		/* Total bytes write to stdout/streamed to application/CGI */
    uint16_t ts_sync_error;				/* Total global number of sync errors */
	uint16_t service_id;				/* The service_id, aka program_id */
	char *programme;					/* the environment variable PROGRAMMNO (no hassling arround with REDIRECT_ ) */
	uint8_t	want_ac3;					/* do we want AC-3 output */
	uint8_t prefer_rds;					/* do we prefer RDS  - instead of EPG? (only if there is RDS) */
	uint8_t found_rds;					/* We found RDS, don't use EIT any longer */
	uint16_t	transport_stream_id;		/* The transport stream id of the wanted programm stream (important for EIT/SDT scan) */
} programm_info_t; 

/* An aggregator, currently used only for EIT (event information table) */
typedef struct section_aggregate_s {
	uint8_t		buffer_valid;		/* Buffer is valid */
	uint8_t		continuation;		/* A continued (EIT) information block is found */
	uint8_t		counter;			/* The counter for the current fragment */
	uint16_t	offset;				/* Determine the offset in the information buffer */
	uint16_t	section_length;		/* The section length as found in the first ts packet */
	uint8_t		buffer[EIT_BUF_SIZE];
	uint8_t		offset_buffer[TS_PACKET_SIZE]; /* A buffer for continued tables */
	uint8_t		ob_used;			/* Pointer wether the offset_buffer is used */
} section_aggregate_t; 

/* crc32.c */
uint32_t crc32 (unsigned char *data, int len);
uint16_t crc16 (unsigned char *data, int len);

/* In ts2shout.c */
void output_logmessage(const char *fmt, ... ); 
extern int channel_count;
extern ts2shout_channel_t *channel_map[MAX_PID_COUNT];
extern ts2shout_channel_t *channels[MAX_CHANNEL_COUNT];

/* process_ts_packet returns the number of handled bytes, 0 or one of the two
 * error codes. A soft error is logged and ignored if it happens spuriously, a 
 * hard error leads to an immediate end of stream processing and an exit with an 
 * appropriate log message. TODO: Handle the SOFT_ERROR */
#define TS_SOFT_ERROR -1
#define TS_HARD_ERROR -2 
int16_t process_ts_packet(unsigned char *buf);

/* In pes.c */
unsigned char* parse_pes( unsigned char* buf, int size, size_t *payload_size, ts2shout_channel_t *chan);

/* In util.c */
void init_structures();
int add_channel(enum_channel_type channel_type, int pid); 
/* Get nice channel_name */
const char* channel_name(enum_channel_type channel_type); 
unsigned char *utf8(unsigned char* in, unsigned char* out); 

void add_cache(programm_info_t* global_state); 
void fetch_cached_parameters(programm_info_t* global_state); 

#endif
