/*
 *  MPEG Audio Header Parser
 *  and rudimentary AC-3 Audio Header Parser
 *
 *  Copyright (C) 2006 Nicholas J. Humfrey
 *  Copyright (C) 2018, 2019 Carsten Groß
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


/*
    MPEG Audio frame handling courtesy of Scott Manley
    Borrowed from libshout 2.1 / mp3.c 
    With a few changes and fixes
 */
 
#include <stdio.h>
#include <stdlib.h>

#include "mpa_header.h"
#include "ts2shout.h"

extern programm_info_t *global_state;

#define MPA_MODE_STEREO		0
#define MPA_MODE_JOINT		1
#define MPA_MODE_DUAL		2
#define MPA_MODE_MONO		3


static const unsigned int mp2_bitrate[3][3][16] =
{
	{ // MPEG 1
		{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0}, // Layer 1
		{0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384, 0}, // Layer 2
		{0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 0}  // Layer 3
	},
	{ // MPEG 2
		{0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256, 0}, // Layer 1
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}, // Layer 2
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}  // Layer 3
	},
	{ // MPEG 2.5
		{0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256, 0}, // Layer 1
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}, // Layer 2
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}  // Layer 3
	}
};

static const unsigned int mp2_samplerate[3][4] =
{
	{ 44100, 48000, 32000, 0 }, // MPEG 1
	{ 22050, 24000, 16000, 0 }, // MPEG 2
	{ 11025, 12000,  8000, 0 }  // MPEG 2.5
};

/* new definitions for AC3
 * from http://stnsoft.com/DVD/ac3hdr.html */
static const unsigned int ac3_samplerate[4] = { 48000,44100,32000,0 };

static const unsigned int ac3_bitrate[] = {
	32,  32, 40, 40, 48, 48, 56, 56,  /* value 0 - 7 */
	64,  64, 80, 80, 96, 96,112,112,  /* value 8 - 15 */
	128,128,160,160,192,192,224,224,  /* value 16 - 23 */
	256,256,320,320,384,384,448,448,  /* value 24 - 31 */
	512,512,576,576,640,640,0,  0,    /* value 32 - 39 */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* value 40 - 55 */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0   /* value 56 - 63 */
}; 

static const unsigned int ac3_channels[] = {
	2,1,2,3,3,4,4,5,0,0,0 
}; 

static const char *ac3_channel_name[] = {
	"Ch1/Ch2 (1+1)", "C (1/0)", "L/R (2/0)", "L/C/R (3/0)", "L/R/S (2/1)", 
	"L/C/R/S (3/1)", "L/R/SL/SR (2/2)", "L/C/R/SL/SR (3/2)" }; 

static void parse_header(mpa_header_t *mh, u_int32_t header)
{
	mh->syncword = (header >> 20) & 0x0fff;

	mh->version = 2 - ((header >> 19) & 0x01);
	if ((mh->syncword & 0x01) == 0)
		mh->version = 3;

	mh->layer = 4-((header >> 17) & 0x03);
	if (mh->layer==4)
		mh->layer=0;
		
	mh->error_protection = ((header >> 16) & 0x01) ? 0 : 1;
	mh->bitrate_index = (header >> 12) & 0x0F;
	mh->samplerate_index = (header >> 10) & 0x03;
	mh->padding = (header >> 9) & 0x01;
	mh->extension = (header >> 8) & 0x01;
	mh->mode = (header >> 6) & 0x03;
	mh->mode_ext = (header >> 4) & 0x03;
	mh->copyright = (header >> 3) & 0x01;
	mh->original = (header >> 2) & 0x01;
	mh->emphasis = header & 0x03;

	if (mh->layer && mh->version) {
		mh->bitrate = mp2_bitrate[mh->version-1][mh->layer-1][mh->bitrate_index];
		mh->samplerate = mp2_samplerate[mh->version-1][mh->samplerate_index];
		/* also set global stuff */
		global_state->br = mh->bitrate; 
		global_state->sr = mh->samplerate; 
	} else {
		mh->bitrate = 0;
		mh->samplerate = 0;
	}

	if (mh->mode == MPA_MODE_MONO)
		mh->channels = 1;
	else
		mh->channels = 2;

	if (mh->version == 1)
		mh->samples = 1152;
	else
		mh->samples = 576;

	if(mh->samplerate)
		mh->framesize = (mh->samples * mh->bitrate * 1000 / mh->samplerate) / 8 + mh->padding;
}


// print out all field values for debuging
void mpa_header_debug( mpa_header_t *mh )
{
	if (mh->version==1)			fprintf(stderr, "  version=MPEG-1\n");
	else if (mh->version==2)	fprintf(stderr, "  version=MPEG-2\n");
	else if (mh->version==3)	fprintf(stderr, "  version=MPEG-2.5\n");
	else 						fprintf(stderr, "  version=unknown\n");

	fprintf(stderr, "  layer=%d\n", mh->layer);
	
	if (mh->mode==MPA_MODE_STEREO)		fprintf(stderr, "  mode=Stereo\n");
	else if (mh->mode==MPA_MODE_JOINT)	fprintf(stderr, "  mode=Joint Stereo\n");
	else if (mh->mode==MPA_MODE_DUAL)	fprintf(stderr, "  mode=Dual\n");
	else if (mh->mode==MPA_MODE_MONO)	fprintf(stderr, "  mode=Mono\n");
	else 								fprintf(stderr, "  mode=unknown\n");

	fprintf(stderr, "  error_protection=%d\n", mh->error_protection);
	fprintf(stderr, "  padding=%d\n", mh->padding);
	fprintf(stderr, "  extension=%d\n", mh->extension);
	fprintf(stderr, "  mode_ext=%d\n", mh->mode_ext);
	fprintf(stderr, "  copyright=%d\n", mh->copyright);
	fprintf(stderr, "  original=%d\n", mh->original);
	fprintf(stderr, "  channels=%d\n", mh->channels);
	fprintf(stderr, "  bitrate=%d\n", mh->bitrate);
	fprintf(stderr, "  samplerate=%d\n", mh->samplerate);
	fprintf(stderr, "  samples=%d\n", mh->samples);
	fprintf(stderr, "  framesize=%d\n", mh->framesize);
}

// concise informational string
void mpa_header_print( mpa_header_t *mh )
{
	char mpeg_std[20]; 
	char mpeg_mode[20];
	if (mh->version==1)			sprintf(mpeg_std, "MPEG-1");
	else if (mh->version==2)	sprintf(mpeg_std, "MPEG-2");
	else if (mh->version==3)	sprintf(mpeg_std, "MPEG-2.5");
	else 						sprintf(mpeg_std, "MPEG-??");
	if (mh->mode==MPA_MODE_STEREO)		sprintf(mpeg_mode, "Stereo");
	else if (mh->mode==MPA_MODE_JOINT)	sprintf(mpeg_mode, "Joint Stereo");
	else if (mh->mode==MPA_MODE_DUAL)	sprintf(mpeg_mode, "Dual");
	else if (mh->mode==MPA_MODE_MONO)	sprintf(mpeg_mode, "Mono");
	output_logmessage("   %s layer %d, %d kbps, %d Hz, %s\n", mpeg_std, mh->layer, mh->bitrate, mh->samplerate, mpeg_mode); 
}

void ac3_header_print (mpa_header_t *mh) 
{
	output_logmessage("  AC-3, %d kbit/s, %d Hz, channels: %s\n", mh->bitrate, mh->samplerate, ac3_channel_name[mh->channel_acmod]);
}

// Parse mpeg audio header
// returns 1 if valid, or 0 if invalid
int mpa_header_parse( const unsigned char* buf, mpa_header_t *mh)
{
	u_int32_t head;
		
	/* Quick check */
	if (buf[0] != 0xFF)
		return 0;

	/* Put the first four bytes into an integer */
	head = ((u_int32_t)buf[0] << 24) | 
		   ((u_int32_t)buf[1] << 16) |
		   ((u_int32_t)buf[2] << 8)  |
		   ((u_int32_t)buf[3]);


	/* fill out the header struct */
	parse_header(mh, head);

	/* check for syncword */
	if ((mh->syncword & 0x0ffe) != 0x0ffe)
		return 0;

	/* make sure layer is sane */
	if (mh->layer == 0)
		return 0;

	/* make sure version is sane */
	if (mh->version == 0)
		return 0;

	/* make sure bitrate is sane */
	if (mh->bitrate == 0)
		return 0;

	/* make sure samplerate is sane */
	if (mh->samplerate == 0)
		return 0;

	return 1;
}

// Parse AC-3 Audio header
// returns 1 if valid, or 0 if invalid
// Header info found at http://stnsoft.com/DVD/ac3hdr.html#bsmod
int ac3_header_parse( const unsigned char* buf, mpa_header_t *mh)
{
	// u_int16_t crc16; 
	/* Quick check */
	if (buf[0] != 0x0b)
		return 0;
	if (buf[1] != 0x77) 
		return 0; 
	
	/* Put the first four bytes into an integer */
	// crc16 = (u_int16_t)buf[2]<<8 | (u_int16_t)buf[3];
	mh->samplerate = ac3_samplerate[((buf[4] >>6)&0x3)];
	mh->bitrate    = ac3_bitrate[(buf[4]&0x3f)]; 
	mh->version	   = buf[5]>>3; 
	mh->channel_acmod = buf[6]>>5; 
	mh->channels   = ac3_channels[mh->channel_acmod]; 

	/* make sure version is sane */
	if (mh->version == 0)
		return 0;

	/* make sure bitrate is sane */
	if (mh->bitrate == 0)
		return 0;

	/* make sure samplerate is sane */
	if (mh->samplerate == 0)
		return 0;

	/* sane values are given, set them */
	global_state->br = mh->bitrate; 
	global_state->sr = mh->samplerate; 

	return 1;
}



