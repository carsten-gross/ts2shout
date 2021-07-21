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

#define MAX_BLOCKNR 255


typedef struct module_buffer {
	uint8_t  valid;                    /* All blocks received, i.e. data valid? */
	uint16_t module_number;            /* Number of data module */
	size_t	 data_size;                /* Overall size of data */
	uint16_t max_blocknr;              /* Maximum number of block */
	uint16_t block_size[MAX_BLOCKNR];  /* the size of the current block */
	char *   buffer[MAX_BLOCKNR];      /* Buffer holding the data in length given in block_size */
} module_buffer_t;

typedef struct server_initiate_buffer {

} server_initiate_buffer_t;


/* Makros for accessing DSM-CC packets */
#define DSMCC_MESSAGE_TYPE(b)       (b[0])
#define DSMCC_TABLE_EXTENSION(b)    (b[3]<<8 | b[4])
#define DSMCC_SECTION_NUMBER(b)     (b[6])
#define DSMCC_LAST_SECTION_NUMBER(b) (b[7])
#define DSMCC_MODULE_ID(b)          (b[20]<<8 | b[21])
#define DSMCC_BLOCKNR(b)            (b[24]<<8 | b[25])

#define DSMCC_MESSAGE_SIZE(b)		(b[18]<<8 | b[19])
#define DSMCC_MESSAGE(b)			(b+26)

#define DSMCC_PROTOCOL_DISCRIMINATOR(b)	(b[8])
#define DSMCC_PROTOCOL_MESSAGE_ID(b) (b[10]<<8 | b[11])
#define DSMCC_TRANSACTION_ID(b)     ((uint32_t)((b[12]<<24) | (b[13]<<16) | (b[14]<<8) | b[15]))
#define DSMCC_DII_MODULE_COUNT(b)	(b[38]<<8 | b[39])
#define DSMCC_DII_MODULES_START(b)  (b+40)

#define DSMCC_MODULE_MODULE_ID(b)       (b[0]<<8 | b[1])
#define DSMCC_MODULE_MODULE_SIZE(b)     (b[2]<<24 | b[3]<<16 | b[4]<<8 | b[5])
#define DSMCC_MODULE_MODULE_VERSION(b)  (b[6])
#define DSMCC_MODULE_INFO_LENGTH(b)     (b[7])
#define DSMCC_MODULE_INFO_DESCRIPTOR(b) (b[8])


/* In dsmcc.c */
void handle_dsmcc_message(unsigned char *buf, size_t len);

#endif
