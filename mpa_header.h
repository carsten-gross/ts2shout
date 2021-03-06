/*
 *  MPEG Audio Header Parser
 *
 *  Copyright (C) 2006 Nicholas J. Humfrey
 *  Copyright (C) 2020 Carsten Gross
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef _MPA_HEADER_H
#define _MPA_HEADER_H

typedef struct {
	uint8_t	sync0;
	uint8_t	sync1;
	uint8_t sync2;
	uint8_t sync3;
	unsigned int syncword;
	unsigned int profile;
	unsigned int layer;
	unsigned int version;
	unsigned int error_protection;
	unsigned int bitrate_index;
	unsigned int samplerate_index;
	unsigned int padding;
	unsigned int extension;
	unsigned int mode;
	unsigned int mode_ext;
	unsigned int copyright;
	unsigned int original;
	unsigned int emphasis;
	unsigned int channels;
	unsigned int channel_acmod;
	unsigned int bitrate;
	unsigned int samplerate;
	unsigned int samples;
	unsigned int framesize;
} mpa_header_t;

#define MPA_MODE_STEREO     0
#define MPA_MODE_JOINT      1
#define MPA_MODE_DUAL       2
#define MPA_MODE_MONO       3

// Get parse the header of a frame of mpeg audio
int mpa_header_parse( const unsigned char* buf, mpa_header_t *mh);

void mpa_header_print( mpa_header_t *mh );

/* AC-3 parsing */
int ac3_header_parse( const unsigned char* buf, mpa_header_t *mh);
void ac3_header_print( mpa_header_t *mh );


#endif
