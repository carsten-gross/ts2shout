/*
 *  MPEG Audio Header Parser
 *  and rudimentary AC-3 Audio Header Parser
 *
 *  Copyright (C) 2006 Nicholas J. Humfrey
 *  Copyright (C) 2018-2021 Carsten Groß
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

#include "ts2shout.h"
#include "mpa_header.h"
#include "rds.h"

extern programm_info_t *global_state;

#define MPA_MODE_STEREO		0
#define MPA_MODE_JOINT		1
#define MPA_MODE_DUAL		2
#define MPA_MODE_MONO		3


static const unsigned int mp2_bitrate[4][4][16] =
{
	{ // MPEG 2.5
		{0,  0,  0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0}, // Layer 4 (does not exist)
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}, // Layer 3
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}, // Layer 2
		{0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256, 0}  // Layer 1
	},
	/* Nix ... reserved */
	{ // MPEG 2.5
		{0,  0,  0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0}, // Layer 4 (does not exist)
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}, // Layer 3
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}, // Layer 2
		{0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256, 0}  // Layer 1
	},
	{ // MPEG 2
		{0,  0,  0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0}, // Layer 4 (does not exist)
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}, // Layer 3
		{0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160, 0}, // Layer 2
		{0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256, 0}  // Layer 1
	},
	{ // MPEG 1
		{0,  0,  0,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 0}, // Layer 4 (does not exist)
		{0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 0}, // Layer 3
		{0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384, 0}, // Layer 2
		{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0}  // Layer 1
	},
};

static const unsigned int mp2_samplerate[4][4] =
{
	{ 11025, 12000,  8000, 0 }, // MPEG 2.5
	{     0,     0,     0, 0 }, // Nix
	{ 22050, 24000, 16000, 0 }, // MPEG 2
	{ 44100, 48000, 32000, 0 }, // MPEG 1
};

/* Stuff for AAC */
/* See https://github.com/mvaneerde/blog/blob/master/scripts/adtsaudioheader.pl */

static const unsigned aac_samplerate[] = {
	96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };

static const char * aac_channel_name[] = {
	"Unknown", "C (1/0)", "L/R (2/0)", "L/C/R (3/0)", "L/C/R/S (4/0)",
	"L/C/R/LS/RS (5/0)" , "L/C/R/LS/RS/LFE (5/1)", "L/C/R/LS/RS/LF/RF/LFE (7/1)"};

// static const char * aac_sub_profile_name[] = {
// 	"Main", "Low complexity", "scaleable sampling rate", "reserved" };

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

/* We use the advantage that we already know wether we get
 * AAC or MPEG. We know this from the PMT */
static void parse_header(const unsigned char* buf, mpa_header_t *mh, u_int32_t header)
{
	mh->syncword = (header >> 20) & 0x0fff;
	/* This is for the RDS scan, makes detection of frame header more robust */
	mh->sync0 = (header>>24) & 0xff;
	mh->sync1 = (header>>16) & 0xff;
	mh->sync2 = (header>>8) & 0xff;
	mh->sync3 = (header) & 0xff;

	/* I changed to map it directly to ease AAC/MPEG handling */
	mh->version = (header >> 18) & 0x03;
	mh->layer = (header >> 17) & 0x03;	
	/* The stream type is set via the flags from the PMT */
	if (global_state->stream_type == AUDIO_MODE_AAC) {
		if (mh->version == 0 && mh->layer == 0) {
			/* AAC / ADTS */
			mh->samplerate_index = (header>>12) & 0x0f;
			mh->samplerate		= aac_samplerate[mh->samplerate_index];
			mh->channel_acmod	= (header>>6) & 0x7;
			/* TODO Oergs.. we have to set something until we implement fetching bitrate for AAC */
			mh->bitrate      = 16;
			global_state->sr = mh->samplerate;
			global_state->br = mh->bitrate;
		}
	} else if ( global_state->stream_type == AUDIO_MODE_AACP) {
		/* TODO UGLY AS HELL */
		/* AAC / HE-AAC / COMPLEX / LATM/LOAS */
		/* AAC LATM is protected with a huge amount of software patents.
		 * Transport in mp2t is without ADTS header. In theory the parameters are
		 * transported in PMT, but it's not documented. Therefore we match on
		 * mp4 / aac raw frame-beginnings and reverse engineer the
		 * parameters in PMT AAC descriptor 0x7c */
		uint8_t bitmask = 0xfc;
		if (buf[0] == global_state->latm_magic1 && ((buf[1] & bitmask) == (global_state->latm_magic2 & bitmask) ) ) {
			// unsigned int profile = 0;
#ifdef DEBUG
			fprintf(stderr, "---------------------- HE-ACC found valid syncword ----------------------------\n");
			DumpHex((unsigned char*)buf, 4);
#endif	
			/* LATM gives SR and BR in meta info, not in stream itself */
			mh->samplerate = global_state->sr;
			/* If Bitrate is coming from DVB information (PMT) use it from there */
			if (global_state->br == 0 ) {
				global_state->br = mh->bitrate;
			}
		 }
	} else 	if (global_state->stream_type == AUDIO_MODE_MPEG) {
		if (mh->version != 0 && mh->layer != 0) {
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
			mh->bitrate = mp2_bitrate[mh->version][mh->layer][mh->bitrate_index];
			// fprintf(stderr, "Accessed bitrate: %d, with version %d, Layer %d, bitrate_index %d\n", mh->bitrate, mh->version, mh->layer, mh->bitrate_index);
			mh->samplerate = mp2_samplerate[mh->version][mh->samplerate_index];
			// fprintf(stderr, "Accessed samplerate: %d, with version %d, samplerate_index %d\n", mh->samplerate, mh->version, mh->samplerate_index);
			/* also set global stuff */
			global_state->br = mh->bitrate;
			global_state->sr = mh->samplerate;
		}
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

// concise informational string
void mpa_header_print( mpa_header_t *mh )
{
	char mpeg_std[20];
	char mpeg_mode[20];
	if (global_state->stream_type == AUDIO_MODE_MPEG) {
		if (mh->version==3)			sprintf(mpeg_std, "MPEG-1");
			else if (mh->version==2)	sprintf(mpeg_std, "MPEG-2");
			else if (mh->version==1)	sprintf(mpeg_std, "MPEG-Unknown");
			else if (mh->version == 0)	sprintf(mpeg_std, "MPEG-2.5");
		if (mh->mode==MPA_MODE_STEREO)		sprintf(mpeg_mode, "Stereo");
			else if (mh->mode==MPA_MODE_JOINT)	sprintf(mpeg_mode, "Joint Stereo");
			else if (mh->mode==MPA_MODE_DUAL)	sprintf(mpeg_mode, "Dual");
			else if (mh->mode==MPA_MODE_MONO)	sprintf(mpeg_mode, "Mono");
		output_logmessage("Synced to %s layer %d, %d kbps, %d Hz, %s\n", mpeg_std, mh->layer, mh->bitrate, mh->samplerate, mpeg_mode);
	} else if ( global_state->stream_type == AUDIO_MODE_AAC ) {
		if (mh->version == 0 && mh->layer == 0) {
			output_logmessage("Synced to AAC, Samplerate %d Hz, Configuration: %s\n", mh->samplerate, aac_channel_name[mh->channel_acmod]);
		}
	} else if ( global_state->stream_type == AUDIO_MODE_AACP ) {
		if ( mh->layer == 0 ) {
			output_logmessage("Synced to HE-AAC, Guessed Samplerate %d Hz, Bitrate %d kBit/s\n", global_state->sr, global_state->br);
		}
	}
}

void ac3_header_print (mpa_header_t *mh)
{
	output_logmessage("Synced to AC-3, %d kbit/s, %d Hz, channels: %s\n", mh->bitrate, mh->samplerate, ac3_channel_name[mh->channel_acmod]);
}

// Parse mpeg audio header
// returns 1 if valid, or 0 if invalid
int mpa_header_parse( const unsigned char* buf, mpa_header_t *mh)
{
	u_int32_t head;

	/* Quick check */
	if (global_state->stream_type != AUDIO_MODE_AACP && buf[0] != 0xFF)
		return 0;

	/* Put the first four bytes into an integer */
	head = ((u_int32_t)buf[0] << 24) |
		   ((u_int32_t)buf[1] << 16) |
		   ((u_int32_t)buf[2] << 8)  |
		   ((u_int32_t)buf[3]);


	/* fill out the header struct */
	parse_header(buf, mh, head);
	/* Sanity checks for MPEG1/2 */
	if (global_state->stream_type == AUDIO_MODE_MPEG) {
		/* check for syncword */
		if ((mh->syncword & 0x0ffe) != 0x0ffe)
			return 0;
		/* make sure layer is sane */
		if (mh->layer == 0 && mh->version != 0)
			return 0;
		/* make sure version is sane */
		if (mh->version == 0 && mh->layer != 0)
			return 0;
		/* make sure bitrate is sane */
		if (mh->bitrate == 0)
			return 0;
		/* make sure samplerate is sane */
		if (mh->samplerate == 0)
			return 0;
		return 1;
	} else {
#ifdef DEBUG
			DumpHex((unsigned char*)buf, 4);
#endif
		if (global_state->stream_type != AUDIO_MODE_AACP && (mh->syncword & 0x0fff) != 0xfff)
			return 0;
		if (mh->layer > 0)
			return 0;
		if (mh->samplerate == 0)
			return 0;
		return 1;
	}
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



