/*
 *  RDS Radio Data System support header
 *
 *  Copyright (C) 2020 Carsten Gross <carsten at siski.de>
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

#ifndef _RDS_HEADER_H
#define _RDS_HEADER_H

// rda_data_scanner
void rds_data_scan( ts2shout_channel_t * chan);
void init_rds(); 
void rds_handle_message(uint8_t* rds_message, uint8_t size);
void DumpHex(const void* data, size_t size);
#endif
