/* 

	pes.c
	(C) Nicholas J Humfrey <njh@aelius.com> 2006.
	(C) Carsten Groß <carsten@siski.de> 2019
	
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
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include "ts2shout.h"

unsigned char* parse_pes( unsigned char* buf, int size, size_t *payload_size, ts2shout_channel_t *chan) 
{
	size_t pes_len = PES_PACKET_LEN(buf);
	size_t pes_header_len = PES_PACKET_HEAD_LEN(buf);
	unsigned char stream_id = PES_PACKET_STREAM_ID(buf);

	if( PES_PACKET_SYNC_BYTE1(buf) != 0x00 ||
		PES_PACKET_SYNC_BYTE2(buf) != 0x00 ||
		PES_PACKET_SYNC_BYTE3(buf) != 0x01 )
	{
		output_logmessage("Invalid PES header (pid: %d).\n", chan->pid);
		return 0;
	}
	
	// Stream IDs in range 0xC0-0xDF are MPEG audio
	// Stream ID 0xbd was seen for AC3 audio
	if( stream_id != chan->pes_stream_id )
	{
		if (stream_id == 0x89 ) {
			// RDS-PES-Stream
		} else if (stream_id != 0xbd
			&& ( stream_id < 0xC0 || stream_id > 0xDF )  ) {
			output_logmessage("Ignoring non-mpegaudio stream ID 0x%x (pid: %d).\n", stream_id, chan->pid);
			return 0;
		}
		if (chan->pes_stream_id == 0) {
			// keep the first stream we see
			chan->pes_stream_id = stream_id;
		} else {
			output_logmessage("Ignoring additional audio stream ID 0x%x (pid: %d).\n", stream_id, chan->pid);
			return 0;
		}
	}
	// Check PES Extension header 
	if( PES_PACKET_SYNC_CODE(buf) != 0x2 )
	{
		output_logmessage("Error: invalid sync code PES extension header (pid: %d).\n", chan->pid);
		return 0;
	}

	// Reject scrambled packets
	if( PES_PACKET_SCRAMBLED(buf) )
	{
		output_logmessage("Error: PES payload is scrambled (pid: %d).\n", chan->pid);
		return 0;
	}

	// Store the presentation timestamp of this PES packet
	if (PES_PACKET_PTS_DTS(buf) & 0x2) {
		chan->pes_ts=PES_PACKET_PTS(buf);
	}
	
	// Store the length of the PES packet payload
	// Original code 
	// chan->pes_remaining = pes_len - (2 + pes_header_len);
	chan->pes_remaining = pes_len - (3 + pes_header_len);

	// Return pointer and length of payload in this TS packet
	*payload_size = size - (9 + pes_header_len);
	return buf+(9+pes_header_len);
}

