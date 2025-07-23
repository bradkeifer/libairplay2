/*
 * AirPlay2 : Client to control an AirPlay device
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * This code has been derived from from https://github.com/music-assistant/libairplay2
 * Earlier works:
 * Copyright (C) 2004 Shiro Ninomiya <shiron@snino.com>
 * Philippe <philippe_44@outlook.com>
 */

#include "airplay2_client.h"
#include "cross_util.h"

/*----------------------------------------------------------------------------*/
uint64_t airplay2cl_get_ntp(struct ntp_s* ntp)
{
	uint64_t time = gettime_us();
	uint32_t seconds = time / (1000 * 1000);
	uint32_t fraction = ((time % (1000 * 1000)) << 32) / (1000 * 1000);

	if (ntp) {
		ntp->seconds = seconds;
		ntp->fraction = fraction;
	}

	return ((uint64_t) seconds << 32) | fraction;
}

