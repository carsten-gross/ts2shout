/*

	util.c
	(C) Carsten Gross <carsten@siski.de> 2018-2021
	
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


#include "ts2shout.h"

#define CACHE_FILENAME "/var/tmp/ts2shout.cache"


static const char *channel_type_name[] = {
    FOREACH_CHANNEL_TYPE(GENERATE_STRING)
};

static const char *mime_type_string[] = { 	"none", "audio/mpeg", "audio/aac", "audio/aacp", "audio/ac3" };

/* initialize the structure after allocating memory for it */
void init_channel (enum_channel_type channel_type, int pid, int current_channel) {
	ts2shout_channel_t *chan =  channels[ current_channel ];
	chan->channel_type = channel_type;
	chan->pid = pid;
    chan->num = current_channel;
	chan->continuity_count = -1;
	channel_map[ pid ] = chan;
	return;
}

/* Add a channel */
int add_channel ( enum_channel_type channel_type, int pid) {
	ts2shout_channel_t *chan = NULL;
	/* Avoid logging the default */
	if ( (pid != 17) && (pid != 18) && (pid != 0) ) {	
		output_logmessage("add_channel(): Subscribing to MPEG-TS PID %d (Type %s)\n", pid, channel_name(channel_type));
	}
	if ( channel_count >= MAX_CHANNEL_COUNT ) {
		fprintf(stderr, "add_channel(): Trying to add more then %d channels\n", MAX_CHANNEL_COUNT);
		return 0;
	}
	if ( channel_map[ pid ] ) {
		fprintf(stderr, "add_channel(): Channel with PID %d already exists\n", pid);
		return 0;
	}

	chan = calloc( 1, sizeof(ts2shout_channel_t) );
	if (!chan) {
		fprintf(stderr, "add_channel(): Failed to allocate memory for new channel with PID %d and channel_type %d", pid, channel_type);
		return 0;
	}
    channels[ channel_count ] = chan;
	init_channel(channel_type, pid, channel_count);
	channel_count++;
	return 1;
}

void init_structures() {
	output_logmessage("init_structures(): Subscribing to MPEG-TS PID 0, 17, 18 (%s, %s, %s)\n",
			channel_name(CHANNEL_TYPE_PAT), channel_name(CHANNEL_TYPE_SDT), channel_name(CHANNEL_TYPE_EIT));
	if (! add_channel(CHANNEL_TYPE_PAT, 0))
		exit(1);
	if (! add_channel(CHANNEL_TYPE_SDT, 17))
		exit(1);
	if (! add_channel(CHANNEL_TYPE_EIT, 18))
		exit(1);
	return;
}

/* Get Name of channel name for better debugging */
const char* channel_name(enum_channel_type channel_type) {
	return channel_type_name[channel_type];
}

/* Get MIME/TYPE for streamed audio-stream */
const char* mime_type(enum_stream_type stream_type) {
	if (stream_type >= 0 && stream_type < 5) {
		return mime_type_string[stream_type];
	} else {
		return "error";
	}
}

/* Get AAC profile / for profile given in AAC profile information */
const char* aac_profile_name(uint8_t profile_and_level) {
	static char profile_name[101];
	int retval;
	profile_name[100] = 0;
	retval = snprintf(profile_name, 100, "Unkown AAC profile (%d)", profile_and_level);
	if ( profile_and_level < 0x10)
		return profile_name;
	if ( profile_and_level < 0x14)
		retval = snprintf(profile_name, 100, "Main profile, Level %d", (profile_and_level & 0x03) + 1 );
	if ( profile_and_level > 0x17 && profile_and_level < 0x1c)
		retval = snprintf(profile_name, 100, "Scalable profile, Level %d", (profile_and_level & 0x03) + 1);
	if ( profile_and_level > 0x1f && profile_and_level < 0x22)
		retval = snprintf(profile_name, 100, "Speech profile, Level %d", (profile_and_level & 0x01) + 1);
	if ( profile_and_level > 0x27 && profile_and_level < 0x2b)
		retval = snprintf(profile_name, 100, "Synthesis profile, Level %d", (profile_and_level & 0x03) + 1 );
	if ( profile_and_level > 0x2f && profile_and_level < 0x38)
		retval = snprintf(profile_name, 100, "HQ audio profile, Level %d", (profile_and_level & 0x07) + 1);
	if ( profile_and_level > 0x37 && profile_and_level < 0x40)
		retval = snprintf(profile_name, 100, "Low delay audio profile, Level %d", (profile_and_level & 0x07) + 1);
	if ( profile_and_level > 0x3f && profile_and_level < 0x44)
		retval = snprintf(profile_name, 100, "Natural audio profile, Level %d", (profile_and_level & 0x03) + 1);
	if ( profile_and_level > 0x47 && profile_and_level < 0x4e)
		retval = snprintf(profile_name, 100, "Mobile audio profile, Level %d", (profile_and_level & 0x07) + 1);
	if ( profile_and_level > 0x4f && profile_and_level < 0x54)
		retval = snprintf(profile_name, 100, "AAC profile, Level %d", ((profile_and_level & 0x03) < 2 ? (profile_and_level & 0x03) : (profile_and_level & 0x03) + 1) + 1  );
	if ( profile_and_level > 0x57 && profile_and_level < 0x54)
		retval = snprintf(profile_name, 100, "HE-AAC profile, Level %d", (profile_and_level & 0x03) + 2);
	if ( profile_and_level > 0x5f && profile_and_level < 0x64)
		retval = snprintf(profile_name, 100, "HE-AACv2 profile, Level %d", (profile_and_level & 0x03) + 2);
	assert(retval > 0);
	return profile_name;
}

/* from https://stackoverflow.com/questions/4059775/convert-iso-8859-1-strings-to-utf-8-in-c-c */
unsigned char *utf8(unsigned char *in, unsigned char *out) {
	uint32_t counter = 0;
	unsigned char *outstart = out;
	while (*in && counter+2 < STR_BUF_SIZE) {
	    if (*in<128) {
			*out++=*in++;
			counter++;
		} else {
			*out++=0xc2+(*in>0xbf), *out++=(*in++&0x3f)+0x80;
			counter += 2;
		}
	}
	*out = 0;
	return outstart;
}
	
void add_cache(programm_info_t *global_state) {
	/* the existing cache file, name given in define CACHE_FILENAME */
	int cachefd = 0;
	FILE* CACHEFILE = NULL;
	/* a new temporary file with unique name to be renamed to be later the new cache */
	int tempfd = 0;
	FILE * TEMPFILE = NULL;
	
	char *tempname = NULL;
	/* for reading with getline */
	size_t t = 0;
	char *gptr = NULL;
	int linesize = 0;

	/* Generate temporary file */
	tempname = alloca(sizeof(CACHE_FILENAME) + 8); /* Trailing \0 character */
	strcpy(tempname, CACHE_FILENAME ".XXXXXX");
	tempfd = mkstemp(tempname);
	if (tempfd <= 0) {
		output_logmessage("add_cache() warning: Cannot create temporary file %s (ignored): %s\n", tempname, strerror(errno));
		return;
	}
	fchmod(tempfd, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
	TEMPFILE = fdopen(tempfd, "w");
	fprintf(TEMPFILE, "# programmno\tbitrate\tstreamrate\tac-3?\tstation_name\tstream_type\n");
	fprintf(TEMPFILE, "%s\t%d\t%d\t%d\t%s\t%d\n",
		global_state->programme,
		global_state->br,
		global_state->sr,
		global_state->want_ac3,
		global_state->station_name,
		global_state->stream_type);
	/* Try to open old file */
	cachefd = open(CACHE_FILENAME, O_RDONLY);
	if (cachefd < 0) {
		output_logmessage("add_cache() warning: Cannot open cache file %s (trying to create): %s\n", CACHE_FILENAME, strerror(errno));
	} else {
		int ac3 = 0;
		CACHEFILE = fdopen(cachefd, "r");
		/* Search for line starting with programme */
		while( (linesize = getline(&gptr, &t, CACHEFILE)) > 0) {
			sscanf(gptr, "%*[^\t]\t%*d\t%*d\t%d\t%*s\t%*d", &ac3);
			if ( (!global_state->programme)
				|| (strncmp(global_state->programme, gptr, strlen(global_state->programme)) != 0)
				|| (ac3 != global_state->want_ac3 )
				)  {
				/* not equal, and not top line */
				if (strncmp("# programmno", gptr, strlen("# programmno")) != 0) {
					fprintf(TEMPFILE, "%s", gptr);
				}
			} else {
				/* done after reading everything */
			}
		}
	}
	fclose(TEMPFILE);
	if (rename(tempname, CACHE_FILENAME) < 0) {
		output_logmessage("add_cache() warning: Cannot rename %s to %s: %s (removing %s anyway)\n", tempname, CACHE_FILENAME, strerror(errno), tempname);
		unlink(tempname);
	}
	if (gptr) {
		free(gptr);
	}
	return;
}

void fetch_cached_parameters(programm_info_t *global_state) {
	FILE* CACHEFILE = NULL;
	/* für getline */
	size_t t = 0;
	char *gptr = NULL;
	int linesize = 0;
	int ac3 = 0;
	if (! global_state->programme) {
		return;
	}
	CACHEFILE = fopen(CACHE_FILENAME, "r");
	if (! CACHEFILE) {
		return;
	}
	while( (linesize = getline(&gptr, &t, CACHEFILE)) > 0) {
		if (strncmp(global_state->programme, gptr, strlen(global_state->programme)) == 0) {
			/* a maximum of 600 characters is absolutly ok and fits into STR_BUF_SIZE */
			sscanf(gptr, "%600[^\t]\t%d\t%d\t%d\t%600[^\t]\t%d[^\n]", global_state->programme, &global_state->br, &global_state->sr, &ac3, global_state->station_name, (int*)&global_state->stream_type);
			if (global_state->want_ac3 == ac3 && global_state->stream_type > 0 ) {
				global_state->mime_type = mime_type(global_state->stream_type);
				output_logmessage("fetch_cached_parameters(): found parameters for programme %s\n", global_state->programme);
				break;
			} else {
				/* reset, because it's not complete or the wrong line if ac3 state is not the same or stream_type is 0 */
				global_state->sr = 0; global_state->br = 0;
				strcpy(global_state->station_name, "");
			}
		}
	}
	if (gptr) {
		free(gptr);
	}
	fclose(CACHEFILE);
	return;
};
