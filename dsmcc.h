/* dsmcc.h

   (C) Carsten Gross <carsten@siski.de> 2021

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

#ifndef _DSMCC_H
#define _DSMCC_H

#include <stdint.h>
#include "mpa_header.h"

typedef struct module_buffer {
	uint8_t	valid; 
	uint16_t module_number;
    uint16_t block_size[255];
	char * buffer[255]; 
} module_buffer_t;

/* Makros for accessing DSM-CC packets */
#define DSMCC_MESSAGE_TYPE(b)       (b[0])
#define DSMCC_TABLE_EXTENSION(b)    (b[3]<<8 | b[4])
#define DSMCC_SECTION_NUMBER(b)     (b[6])
#define DSMCC_LAST_SECTION_NUMBER(b) (b[7])
#define DSMCC_MODULE_ID(b)          (b[20]<<8 | b[21])
#define DSMCC_BLOCKNR(b)            (b[24]<<8 | b[25])

#define DSMCC_MESSAGE_SIZE(b)		(b[18]<<8 | b[19])
#define DSMCC_MESSAGE(b)			(b+26)

/* In dsmcc.c */
void handle_dsmcc_message(unsigned char *buf, size_t len);

#endif
